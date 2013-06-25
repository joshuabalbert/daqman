/** @defgroup daqman daqman - acquire raw data from digitizers
 *  
 *  @file daqman.cc
 *  @brief main file for the daqman exectutable.
 *  
 *  daqman acquires raw data from the digitizers.  Automatically loads the file
 *  daqman.cfg.  Several comman-line arguments are also available to 
 *  affect the trigger params. 
 *
 *  @ingroup daqman
 */

#include "V172X_Daq.hh"
#include "V172X_Event.hh"
#include "ConfigHandler.hh"
#include "CommandSwitchFunctions.hh"
#include "EventHandler.hh"
#include "AsyncEventHandler.hh"
#include "RawWriter.hh"
#include "ProcessedPlotter.hh"
#include "Reader.hh"

#include "GenericAnalysis.hh"
#include "BaselineFinder.hh"
#include "PulseFinder.hh"
#include "Integrator.hh"
#include "EvalRois.hh"
#include "SpectrumMaker.hh"
#include "ConvertData.hh"
#include "SumChannels.hh"
#include "TriggerHistory.hh"
#include "RunDB.hh"
#include "kbhit.h"
#include <exception>
#include <string>
#include "Message.hh"
#include <time.h>
#include <fstream>
#include <algorithm>
#include <numeric>
#include "boost/thread/thread.hpp"
bool stop_run = false;

//utility class to print run statistics
struct PrintStats{
  time_t last_print_time;
  unsigned long events_processed;
  unsigned long long bytes_processed;
  void Clear() { 
    last_print_time = time(0); 
    events_processed = 0;
    bytes_processed = 0;
  }
  PrintStats() { Clear(); }
};
std::ostream& operator<<(std::ostream& out, const PrintStats& stats){
  time_t now = time(0);
  out<<"In last "<<now-stats.last_print_time<<" seconds, processed "
     <<stats.events_processed<<" events at "
     <<stats.bytes_processed/(now-stats.last_print_time)/1024<<" kiB/s";
  return out;
}

//utility class to add real-time spectra
class SpectrumAdder{
  AsyncEventHandler* _handler;
public:
  SpectrumAdder(AsyncEventHandler* handler) : _handler(handler) {}
  std::istream& operator()(std::istream& in){
    //first parameter is the name of the spectrum module
    std::string name;
    in>>name;
    SpectrumMaker* spec = new SpectrumMaker(name);
    //expect to read parameters immediately after name
    in>>*spec;
    //register the spectrum with the event handler
    _handler->AddModule(spec);
    return in;
  }
};
  

int main(int argc, char** argv)
{
  
  //set up the config handler
  ConfigHandler* config = ConfigHandler::GetInstance();
  
  //register all processing modules
  EventHandler* modules = EventHandler::GetInstance();
  //only RawWriter and reading headers go synchronously 
  RawWriter* writer = modules->AddModule<RawWriter>();
  ConvertData* read_headers = modules->AddModule<ConvertData>("ReadHeaders");
  read_headers->SetHeadersOnly(true);
  
  std::vector<AsyncEventHandler*> async_threads;
  
  //All analyzing modules run on a single asynchronous thread
  AsyncEventHandler thread1;
  modules->AddAsyncReceiver(&thread1);
  thread1.AddModule(new ConvertData);
  thread1.AddModule(new SumChannels);
  thread1.AddModule(new BaselineFinder);
  thread1.AddModule(new Integrator);
  thread1.AddModule(new EvalRois);
  async_threads.push_back(&thread1);
  // any more advanced functions?
  
  //ProcessedPlotter and RootGraphix share a thread and refresh speed
  AsyncEventHandler thread2;
  //add RootGraphix first so dependencies pass, but call afterward...
  RootGraphix* rootgraphix = new RootGraphix;
  modules->AddModule(rootgraphix, false, true);
  thread1.AddReceiver(&thread2);
  ProcessedPlotter* plotter = new ProcessedPlotter;
  thread2.AddModule(plotter);
  thread2.AddModule(new TriggerHistory);
  thread2.AddModule(rootgraphix, false);
  async_threads.push_back(&thread2);
  
  //for right now, put the spectra all on one thread
  AsyncEventHandler thread3;
  thread1.AddReceiver(&thread3);
  //have 3 spectra available but disabled by default
  SpectrumMaker* spec = new SpectrumMaker("Spectrum1");
  spec->enabled = false;
  thread3.AddModule(spec);
  spec = new SpectrumMaker("Spectrum2");
  spec->enabled = false;
  thread3.AddModule(spec);
  spec = new SpectrumMaker("Spectrum3");
  spec->enabled = false;
  thread3.AddModule(spec);
  
  
  //this can break stuff, but it can be useful, so keep it for now
  modules->RegisterReadFunction("add_spectrum", SpectrumAdder(&thread3),
				"Add a new real-time spectrum to display");
  async_threads.push_back(&thread3);
    
  //initialize some options for command switches
  long stop_events = -1, stop_time = -1; 
  long long stop_size = -1;
  int stattime=0;
  bool write_only = false;
  RunDB::runinfo* info = modules->GetRunInfo();
  std::string testmode_file="";
  int graphics_refresh = 1;
  bool require_comment = false;
  config->AddCommandSwitch('i', "info", "Set run database info to <info>",
			   CommandSwitch::DefaultRead<RunDB::runinfo>(*info),
			   "info");
  config->AddCommandSwitch('m',"message","Set database comment to <message>",
			   CommandSwitch::DefaultRead<std::string>(info->comment),
			   "message");
			   
  config->AddCommandSwitch('e',"stop_events","Stop after <n> events",
			   CommandSwitch::DefaultRead<long>(stop_events),
			   "n");
  config->AddCommandSwitch('t',"stop_time","Stop after <n> seconds",
			   CommandSwitch::DefaultRead<long>(stop_time),
			   "n");
  config->AddCommandSwitch('s',"stop_size","Stop after saving <size> MB",
			   CommandSwitch::DefaultRead<long long>(stop_size),
			   "size");
  config->AddCommandSwitch(' ',"write-only",
			   "Disable all modules except for the RawWriter",
			   CommandSwitch::SetValue<bool>(write_only, true));
  config->AddCommandSwitch(' ',"testmode","Fake DAQ from input file <file>",
			   CommandSwitch::DefaultRead<std::string>
			   (testmode_file) ,"file");
  config->AddCommandSwitch(' ',"stat-time","Print stats every <secs> seconds",
			   CommandSwitch::DefaultRead<int>(stattime),"secs");
  config->AddCommandSwitch(' ',"refresh","Time in s between graphics update",
			   CommandSwitch::DefaultRead<int>(graphics_refresh),
			   "secs");
  config->RegisterParameter("stop_size",stop_size,
			    "Maximum file size in bytes before abort the run");
  config->RegisterParameter("stop_time",stop_time,
			    "Maximum time in seconds before we abort the run");
  config->RegisterParameter("stop_events",stop_events,
			    "Maximum number of events before we abort the run");
  config->RegisterParameter("stat-time",stattime,
			    "Time between printing of event/data rates");
  config->RegisterParameter("require_comment", require_comment,
			    "Require a runinfo comment for the run to proceed");
  
  V172X_Daq daq;
    
  config->SetProgramUsageString("daqman [options]");
  config->SetDefaultCfgFile("cfg/daqman.cfg");
  config->ProcessCommandLine(argc,argv);
  if(config->GetNCommandArgs()){
    Message(ERROR)<<"Too many arguments specified."<<std::endl;
    config->PrintSwitches(true);
  }
  
  
  if(writer->enabled){
    modules->SetRunIDFromFilename(writer->GetFilename());
    //only require comment if saving
    if(require_comment){
      while(info->comment == ""){
	//sleep a bit to clear the message queue
	boost::this_thread::sleep(boost::posix_time::millisec(100));
	std::cout<<"Please enter a descriptive comment for this run:"
		 <<std::endl;
	std::getline(std::cin, info->comment);
      }
    }
  
  }
  //see if we want to disable everything
  if(write_only){
    std::vector<BaseModule*>* mods = modules->GetListOfModules();
    for(size_t i=0; i < mods->size(); i++){
      if(mods->at(i) != writer) mods->at(i)->enabled = false;
    }
  }
  
  //set the graphics refresh time
  thread2.SetSleepMillisec(graphics_refresh*1000);
  
  //see if we're in test mode
  Reader* reader = 0;
  if(testmode_file!=""){
    reader = new Reader(testmode_file);
    if(!reader->IsOk()){
      Message(ERROR)<<"Testmode specified, but can't load file "
		    <<testmode_file<<".\n";
      return 1;
    }
  }
  
  //set the terminal into unbuffered mode
  keyboard board;
  //initialize all modules
  Message(INFO)<<"Initializing modules...\n";
  if(modules->Initialize()){
    Message(CRITICAL)<<"Unable to initialize all modules.\n";
    return 1;
  }
  //start up the asynchronous event handlers
  for(size_t i=0; i<async_threads.size(); ++i)
    async_threads[i]->StartRunning();
  try
    {
      if(!reader){
	Message(INFO)<<"Initializing DAQ...\n";
	if(daq.Initialize() == 0){
	  Message(INFO)<<"Starting Run...\n";
	  daq.StartRun();
	}
	else{
	  Message(CRITICAL)<<"Initialization Error!"<<std::endl;
	  modules->Finalize();
	  return -1;
	}
      }
      //start the thread which takes commands
      time_t start_time = time(0);
      long events_downloaded = 0;
      long long data_downloaded = 0;
      PrintStats stats;
      while(!stop_run){
	//see if we should end the run
	if( (stop_time > 0 && time(0)-start_time >= stop_time) ||
	    (stop_events > 0 && events_downloaded >= stop_events) ||
	    (stop_size > 0 && writer->GetBytesWritten()/1000000 >= stop_size) ){
	  stop_run = true;
	}
	//process user input
	if(board.kbhit()){
	  //process user entered command
	  char c = board.getch();
	  switch(c){
	  case 'q':
	  case 'Q':
	    stop_run = true;
	    break;
	  case 'p':
	  case 'P':
	    plotter->TogglePause();
	    break;
	  default:
	    Message(ERROR)<<"Unknown control character '"<<c<<"'\n";
	  };
	}
	if(stop_run) break;
	//get the next event
	RawEventPtr evt;
	if(reader)
	  evt = reader->GetNextEvent();
	else
	  evt = daq.GetNextEvent(500000);
	if(!evt){
	  //see if there's an error
	  if(reader){
	    Message(INFO)<<"Reached end of file, exiting testmode.\n";
	    stop_run = true;
	    break;
	  }
	  else if(daq.GetStatus() != BaseDaq::NORMAL){
	    Message(ERROR)<<"An error occurred while getting next event.\n";
	    Message(ERROR)<<"Attempting to abort run...\n";
	    stop_run = true;
	    break;
	  }
	  else{
	    Message(DEBUG)<<"Waiting for new event ready in memory...\n";
	    continue;
	  }
	}
	if(modules->Process(evt)){
	  Message(ERROR)<<"Problem encountered processing event.\n";
	  //stop_run = true;
	  continue;
	}

	data_downloaded += evt->GetDataSize();
	events_downloaded++;
	stats.events_processed++;
	stats.bytes_processed += evt->GetDataSize();
	if(stattime>0 && time(0)-stats.last_print_time >= stattime){
	  Message(INFO)<<stats<<std::endl;
	  stats.Clear();
	}
      }
      Message(INFO)<<"Ending Run....\n";
      if(!reader){
	Message(DEBUG)<<"Processing remaining events in queue...\n";
	daq.EndRun();
	RawEventPtr evt;
	while((stop_events<=0 || events_downloaded < stop_events) &&
	      daq.GetStatus() == BaseDaq::NORMAL &&
	      (evt = daq.GetNextEvent()) && 
	      !modules->Process(evt)){
	  data_downloaded += evt->GetDataSize();
	  events_downloaded++;
	  stats.events_processed++;
	  stats.bytes_processed += evt->GetDataSize();
	  
	}
      }
      time_t delta_time = time(0)-start_time;
      //end the asyncronous threads
      for(size_t i=0; i<async_threads.size(); ++i)
	async_threads[i]->StopRunning();
      modules->Finalize();
      //print out some statistics
      Message(INFO)<<events_downloaded<<" events processed.\n";
      Message(INFO)<<delta_time<<" seconds elapsed.\n";
      if(delta_time>0){
	Message(INFO)<<data_downloaded<<" total bytes processed at "
		     <<data_downloaded/(delta_time)
		     <<" bytes/s\n";
      }
    }
  catch(std::exception &e)
    {
      std::cerr<<"Caught Exception: "<<e.what()<<"\n";
    }
  
  return daq.GetStatus();
}