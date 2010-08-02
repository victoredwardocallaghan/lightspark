/**************************************************************************
    Lightspark, a free flash player implementation

    Copyright (C) 2009,2010  Alessandro Pignotti (a.pignotti@sssup.it)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include <string>
#include <sstream>
#include <pthread.h>
#include <algorithm>
#include <SDL.h>
#include "abc.h"
#include "flashdisplay.h"
#include "flashevents.h"
#include "swf.h"
#include "logger.h"
#include "actions.h"
#include "streams.h"
#include "asobjects.h"
#include "textfile.h"
#include "class.h"
#include "netutils.h"

#include <GL/glew.h>
#ifdef ENABLE_CURL
#include <curl/curl.h>

#include "compat.h"
#endif
#ifdef ENABLE_LIBAVCODEC
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif
#ifndef WIN32
#include <GL/glx.h>
#include <fontconfig/fontconfig.h>
#endif

#ifdef COMPILE_PLUGIN
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#endif

using namespace std;
using namespace lightspark;

extern TLSDATA SystemState* sys;
extern TLSDATA RenderThread* rt;
extern TLSDATA ParseThread* pt;

SWF_HEADER::SWF_HEADER(istream& in):valid(false)
{
	in >> Signature[0] >> Signature[1] >> Signature[2];

	in >> Version >> FileLength;
	if(Signature[0]=='F' && Signature[1]=='W' && Signature[2]=='S')
	{
		LOG(LOG_NO_INFO, "Uncompressed SWF file: Version " << (int)Version << " Length " << FileLength);
	}
	else if(Signature[0]=='C' && Signature[1]=='W' && Signature[2]=='S')
	{
		LOG(LOG_NO_INFO, "Compressed SWF file: Version " << (int)Version << " Length " << FileLength);
	}
	else
	{
		LOG(LOG_NO_INFO,"No SWF file signature found");
		return;
	}
	pt->version=Version;
	in >> FrameSize >> FrameRate >> FrameCount;
	float frameRate=FrameRate;
	frameRate/=256;
	LOG(LOG_NO_INFO,"FrameRate " << frameRate);

	pt->root->setFrameRate(frameRate);
	//TODO: setting render rate should be done when the clip is added to the displaylist
	sys->setRenderRate(frameRate);
	pt->root->version=Version;
	pt->root->fileLenght=FileLength;
	valid=true;
}

RootMovieClip::RootMovieClip(LoaderInfo* li, bool isSys):initialized(false),parsingIsFailed(false),frameRate(0),mutexFrames("mutexFrame"),
	toBind(false),mutexChildrenClips("mutexChildrenClips")
{
	root=this;
	sem_init(&mutex,0,1);
	sem_init(&new_frame,0,0);
	sem_init(&sem_valid_size,0,0);
	sem_init(&sem_valid_rate,0,0);
	loaderInfo=li;
	//Reset framesLoaded, as there are still not available
	framesLoaded=0;

	//We set the protoype to a generic MovieClip
	if(!isSys)
		setPrototype(Class<MovieClip>::getClass());
}

RootMovieClip::~RootMovieClip()
{
	sem_destroy(&mutex);
	sem_destroy(&new_frame);
	sem_destroy(&sem_valid_rate);
	sem_destroy(&sem_valid_size);
}

void RootMovieClip::parsingFailed()
{
	//The parsing is failed, we have no change to be ever valid
	parsingIsFailed=true;
	sem_post(&new_frame);
	sem_post(&sem_valid_size);
	sem_post(&sem_valid_rate);
}

void RootMovieClip::bindToName(const tiny_string& n)
{
	assert_and_throw(toBind==false);
	toBind=true;
	bindName=n;
}

void RootMovieClip::registerChildClip(MovieClip* clip)
{
	Locker l(mutexChildrenClips);
	clip->incRef();
	childrenClips.insert(clip);
}

void RootMovieClip::unregisterChildClip(MovieClip* clip)
{
	Locker l(mutexChildrenClips);
	childrenClips.erase(clip);
	clip->decRef();
}

SystemState::SystemState(ParseThread* p):RootMovieClip(NULL,true),parseThread(p),renderRate(0),error(false),shutdown(false),
	renderThread(NULL),inputThread(NULL),engine(NONE),fileDumpAvailable(0),waitingForDump(false),vmVersion(VMNONE),childPid(0),
	useGnashFallback(false),showProfilingData(false),showInteractiveMap(false),showDebug(false),xOffset(0),yOffset(0),currentVm(NULL),
	finalizingDestruction(false),useInterpreter(true),useJit(false),downloadManager(NULL)
{
	//Do needed global initialization
#ifdef ENABLE_CURL
	curl_global_init(CURL_GLOBAL_ALL);
#endif
#ifdef ENABLE_LIBAVCODEC
	avcodec_register_all();
#endif

	cookiesFileName[0]=0;
	//Create the thread pool
	sys=this;
	sem_init(&terminated,0,0);

	//Get starting time
	if(parseThread) //ParseThread may be null in tightspark
		parseThread->root=this;
	threadPool=new ThreadPool(this);
	timerThread=new TimerThread(this);
	audioManager=new AudioManager;
	loaderInfo=Class<LoaderInfo>::getInstanceS();
	stage=Class<Stage>::getInstanceS();
	parent=stage;
	startTime=compat_msectiming();
	
	setPrototype(Class<MovieClip>::getClass());

	setOnStage(true);
}

void SystemState::setDownloadedPath(const tiny_string& p)
{
	dumpedSWFPath=p;
	sem_wait(&mutex);
	if(waitingForDump)
		fileDumpAvailable.signal();
	sem_post(&mutex);
}

void SystemState::setUrl(const tiny_string& url)
{
	loaderInfo->url=url;
	loaderInfo->loaderURL=url;
}

int SystemState::hexToInt(char c)
{
	if(c>='0' && c<='9')
		return c-'0';
	else if(c>='a' && c<='f')
		return c-'a'+10;
	else if(c>='A' && c<='F')
		return c-'A'+10;
	else
		return -1;
}

void SystemState::setCookies(const char* c)
{
	rawCookies=c;
}

void SystemState::parseParametersFromFlashvars(const char* v)
{
	if(useGnashFallback) //Save a copy of the string
		rawParameters=v;
	ASObject* params=Class<ASObject>::getInstanceS();
	//Add arguments to SystemState
	string vars(v);
	uint32_t cur=0;
	while(cur<vars.size())
	{
		int n1=vars.find('=',cur);
		if(n1==-1) //Incomplete parameters string, ignore the last
			break;

		int n2=vars.find('&',n1+1);
		if(n2==-1)
			n2=vars.size();

		string varName=vars.substr(cur,(n1-cur));

		//The variable value has to be urldecoded
		bool ok=true;
		string varValue;
		varValue.reserve(n2-n1); //The maximum lenght
		for(int j=n1+1;j<n2;j++)
		{
			if(vars[j]!='%')
				varValue.push_back(vars[j]);
			else
			{
				if((n2-j)<3) //Not enough characters
				{
					ok=false;
					break;
				}

				int t1=hexToInt(vars[j+1]);
				int t2=hexToInt(vars[j+2]);
				if(t1==-1 || t2==-1)
				{
					ok=false;
					break;
				}

				int c=(t1*16)+t2;
				varValue.push_back(c);
				j+=2;
			}
		}

		if(ok)
		{
			//cout << varName << ' ' << varValue << endl;
			params->setVariableByQName(varName.c_str(),"",
					lightspark::Class<lightspark::ASString>::getInstanceS(varValue));
		}
		cur=n2+1;
	}
	setParameters(params);
}

void SystemState::parseParametersFromFile(const char* f)
{
	ifstream i(f);
	if(!i)
	{
		LOG(LOG_ERROR,"Parameters file not found");
		return;
	}
	ASObject* ret=Class<ASObject>::getInstanceS();
	while(!i.eof())
	{
		string name,value;
		getline(i,name);
		getline(i,value);

		ret->setVariableByQName(name,"",Class<ASString>::getInstanceS(value));
	}
	setParameters(ret);
	i.close();
}

void SystemState::setParameters(ASObject* p)
{
	loaderInfo->setVariableByQName("parameters","",p);
}

void SystemState::stopEngines()
{
	//Stops the thread that is parsing us
	parseThread->stop();
	parseThread->wait();
	threadPool->stop();
	if(timerThread)
		timerThread->wait();
	delete downloadManager;
	downloadManager=NULL;
	delete currentVm;
	currentVm=NULL;
	delete timerThread;
	timerThread=NULL;
	delete audioManager;
	audioManager=NULL;
}

SystemState::~SystemState()
{
	//Kill our child process if any
	if(childPid)
	{
		kill_child(childPid);
	}
	//Delete the temporary cookies file
	if(cookiesFileName[0])
		unlink(cookiesFileName);
	assert(shutdown);
	//The thread pool should be stopped before everything
	delete threadPool;
	stopEngines();

	//decRef all our object before destroying classes
	Variables.destroyContents();
	loaderInfo->decRef();
	loaderInfo=NULL;

	//We are already being destroyed, make our prototype abandon us
	setPrototype(NULL);
	
	//Destroy the contents of all the classes
	std::map<tiny_string, Class_base*>::iterator it=classes.begin();
	for(;it!=classes.end();++it)
		it->second->cleanUp();

	finalizingDestruction=true;
	
	//Also destroy all frames
	frames.clear();

	//Destroy all registered classes
	it=classes.begin();
	for(;it!=classes.end();++it)
	{
		//DEPRECATED: to force garbage collection we delete all the classes
		delete it->second;
		//it->second->decRef()
	}

	//Also destroy all tags
	for(unsigned int i=0;i<tagsStorage.size();i++)
		delete tagsStorage[i];

	delete renderThread;
	delete inputThread;
	sem_destroy(&terminated);
}

bool SystemState::isOnError() const
{
	return error;
}

bool SystemState::isShuttingDown() const
{
	return shutdown;
}

bool SystemState::shouldTerminate() const
{
	return shutdown || error;
}

void SystemState::setError(const string& c)
{
	//We record only the first error for easier fix and reporting
	if(!error)
	{
		error=true;
		errorCause=c;
		timerThread->stop();
		if(renderThread)
		{
			//Disable timed rendering
			removeJob(renderThread);
			renderThread->draw();
		}
	}
}

void SystemState::setShutdownFlag()
{
	sem_wait(&mutex);
	shutdown=true;
	if(currentVm)
		currentVm->addEvent(NULL,new ShutdownEvent());

	sem_post(&terminated);
	sem_post(&mutex);
}

void SystemState::wait()
{
	sem_wait(&terminated);
	if(renderThread)
		renderThread->wait();
	if(inputThread)
		inputThread->wait();
}

float SystemState::getRenderRate()
{
	return renderRate;
}

void SystemState::startRenderTicks()
{
	assert(renderThread);
	assert(renderRate);
	removeJob(renderThread);
	addTick(1000/renderRate,renderThread);
}

void SystemState::EngineCreator::execute()
{
	sys->createEngines();
}

void SystemState::EngineCreator::threadAbort()
{
	assert(sys->shutdown);
	sys->fileDumpAvailable.signal();
}

#ifndef GNASH_PATH
#error No GNASH_PATH defined
#endif

void SystemState::enableGnashFallback()
{
	//Check if the gnash standalone executable is available
	ifstream f(GNASH_PATH);
	if(f)
		useGnashFallback=true;
	f.close();
}

#ifdef COMPILE_PLUGIN

void SystemState::delayedCreation(SystemState* th)
{
	NPAPI_params& p=th->npapiParams;
	//Create a plug in the XEmbed window
	p.container=gtk_plug_new((GdkNativeWindow)p.window);
	//Realize the widget now, as we need the X window
	gtk_widget_realize(p.container);
	//Show it now
	gtk_widget_show(p.container);
	gtk_widget_map(p.container);
	p.window=GDK_WINDOW_XWINDOW(p.container->window);
	XSync(p.display, False);
	sem_wait(&th->mutex);
	th->renderThread=new RenderThread(th, th->engine, &th->npapiParams);
	th->inputThread=new InputThread(th, th->engine, &th->npapiParams);
	//If the render rate is known start the render ticks
	if(th->renderRate)
		th->startRenderTicks();
	sem_post(&th->mutex);
}

#endif

void SystemState::createEngines()
{
	sem_wait(&mutex);
	assert(renderThread==NULL && inputThread==NULL);
#ifdef COMPILE_PLUGIN
	//Check if we should fall back on gnash
	if(useGnashFallback && engine==GTKPLUG && vmVersion!=AVM2)
	{
		if(dumpedSWFPath.len()==0) //The path is not known yet
		{
			waitingForDump=true;
			sem_post(&mutex);
			fileDumpAvailable.wait();
			if(shutdown)
				return;
			sem_wait(&mutex);
		}
		LOG(LOG_NO_INFO,"Invoking gnash!");
		//Dump the cookies to a temporary file
		strcpy(cookiesFileName,"/tmp/lightsparkcookiesXXXXXX");
		int file=mkstemp(cookiesFileName);
		if(file!=-1)
		{
			write(file,"Set-Cookie: ", 12);
			write(file,rawCookies.c_str(),rawCookies.size());
			close(file);
			setenv("GNASH_COOKIES_IN", cookiesFileName, 1);
		}
		else
			cookiesFileName[0]=0;
		childPid=fork();
		if(childPid==-1)
		{
			LOG(LOG_ERROR,"Child process creation failed, lightspark continues");
			childPid=0;
		}
		else if(childPid==0) //Child process scope
		{
			//Allocate some buffers to store gnash arguments
			char bufXid[32];
			char bufWidth[32];
			char bufHeight[32];
			snprintf(bufXid,32,"%lu",npapiParams.window);
			snprintf(bufWidth,32,"%u",npapiParams.width);
			snprintf(bufHeight,32,"%u",npapiParams.height);
			string params("FlashVars=");
			params+=rawParameters;
			char *const args[] =
			{
				strdup("gnash"), //argv[0]
				strdup("-x"), //Xid
				bufXid,
				strdup("-j"), //Width
				bufWidth,
				strdup("-k"), //Height
				bufHeight,
				strdup("-u"), //SWF url
				strdup(origin.raw_buf()),
				strdup("-P"), //SWF parameters
				strdup(params.c_str()),
				strdup("-vv"),
				strdup(dumpedSWFPath.raw_buf()), //SWF file
				NULL
			};
			execve(GNASH_PATH, args, environ);
			//If we are are execve failed, print an error and die
			LOG(LOG_ERROR,"Execve failed, content will not be rendered");
			exit(0);
		}
		else //Parent process scope
		{
			sem_post(&mutex);
			//Engines should not be started, stop everything
			stopEngines();
			return;
		}
	}
#else 
	//COMPILE_PLUGIN not defined
	throw new UnsupportedException("GNASH fallback not available when not built with COMPILE_PLUGIN");
#endif

	if(engine==GTKPLUG) //The engines must be created int the context of the main thread
	{
#ifdef COMPILE_PLUGIN
		npapiParams.helper(npapiParams.helperArg, (helper_t)delayedCreation, this);
#else
		throw new UnsupportedException("Plugin engine not available when not built with COMPILE_PLUGIN");
#endif
	}
	else //SDL engine
	{
		renderThread=new RenderThread(this, engine, NULL);
		inputThread=new InputThread(this, engine, NULL);
		//If the render rate is known start the render ticks
		if(renderRate)
			startRenderTicks();
	}
	sem_post(&mutex);
}

void SystemState::needsAVM2(bool n)
{
	sem_wait(&mutex);
	assert(currentVm==NULL);
	//Create the virtual machine if needed
	if(n)
	{
		vmVersion=AVM2;
		LOG(LOG_NO_INFO,"Creating VM");
		currentVm=new ABCVm(this);
	}
	else
		vmVersion=AVM1;
	if(engine)
		addJob(new EngineCreator);
	sem_post(&mutex);
}

void SystemState::setParamsAndEngine(ENGINE e, NPAPI_params* p)
{
	sem_wait(&mutex);
	if(p)
		npapiParams=*p;
	engine=e;
	if(vmVersion)
		addJob(new EngineCreator);
	sem_post(&mutex);
}

void SystemState::setRenderRate(float rate)
{
	sem_wait(&mutex);
	if(renderRate>=rate)
	{
		sem_post(&mutex);
		return;
	}
	
	//The requested rate is higher, let's reschedule the job
	renderRate=rate;
	if(renderThread)
		startRenderTicks();
	sem_post(&mutex);
}

void SystemState::tick()
{
	RootMovieClip::tick();
 	sem_wait(&mutex);
	list<ThreadProfile>::iterator it=profilingData.begin();
	for(;it!=profilingData.end();it++)
		it->tick();
	sem_post(&mutex);
	//Enter frame should be sent to the stage too
	if(stage->hasEventListener("enterFrame"))
	{
		Event* e=Class<Event>::getInstanceS("enterFrame");
		getVm()->addEvent(stage,e);
		e->decRef();
	}
}

void SystemState::addJob(IThreadJob* j)
{
	threadPool->addJob(j);
}

void SystemState::addTick(uint32_t tickTime, ITickJob* job)
{
	timerThread->addTick(tickTime,job);
}

void SystemState::addWait(uint32_t waitTime, ITickJob* job)
{
	timerThread->addWait(waitTime,job);
}

bool SystemState::removeJob(ITickJob* job)
{
	return timerThread->removeJob(job);
}

ThreadProfile* SystemState::allocateProfiler(const lightspark::RGB& color)
{
	sem_wait(&mutex);
	profilingData.push_back(ThreadProfile(color,100));
	ThreadProfile* ret=&profilingData.back();
	sem_post(&mutex);
	return ret;
}

void ThreadProfile::setTag(const std::string& t)
{
	Locker locker(mutex);
	if(data.empty())
		data.push_back(ProfilingData(tickCount,0));
	
	data.back().tag=t;
}

void ThreadProfile::accountTime(uint32_t time)
{
	Locker locker(mutex);
	if(data.empty() || data.back().index!=tickCount)
		data.push_back(ProfilingData(tickCount, time));
	else
		data.back().timing+=time;
}

void ThreadProfile::tick()
{
	Locker locker(mutex);
	tickCount++;
	//Purge first sample if the second is already old enough
	if(data.size()>2 && (tickCount-data[1].index)>uint32_t(len))
	{
		if(!data[0].tag.empty() && data[1].tag.empty())
			data[0].tag.swap(data[1].tag);
		data.pop_front();
	}
}

void ThreadProfile::plot(uint32_t maxTime, FTFont* font)
{
	if(data.size()<=1)
		return;

	Locker locker(mutex);
	RECT size=sys->getFrameSize();
	int width=size.Xmax/20;
	int height=size.Ymax/20;
	
	int32_t start=tickCount-len;
	if(int32_t(data[0].index-start)>0)
		start=data[0].index;
	
	glPushAttrib(GL_TEXTURE_BIT | GL_LINE_BIT);
	glColor3ub(color.Red,color.Green,color.Blue);
	glDisable(GL_TEXTURE_2D);
	glLineWidth(2);

	glBegin(GL_LINE_STRIP);
		for(unsigned int i=0;i<data.size();i++)
		{
			int32_t relx=int32_t(data[i].index-start)*width/len;
			glVertex2i(relx,data[i].timing*height/maxTime);
		}
	glEnd();
	glPopAttrib();
	
	//Draw tags
	string* curTag=NULL;
	int curTagX=0;
	int curTagY=maxTime;
	int curTagLen=0;
	int curTagH=0;
	for(unsigned int i=0;i<data.size();i++)
	{
		int32_t relx=int32_t(data[i].index-start)*width/len;
		if(!data[i].tag.empty())
		{
			//New tag, flush the old one if present
			if(curTag)
				font->Render(curTag->c_str() ,-1,FTPoint(curTagX,imax(curTagY-curTagH,0)));
			//Measure tag
			FTBBox tagBox=font->BBox(data[i].tag.c_str(),-1);
			curTagLen=(tagBox.Upper()-tagBox.Lower()).X();
			curTagH=(tagBox.Upper()-tagBox.Lower()).Y();
			curTag=&data[i].tag;
			curTagX=relx;
			curTagY=maxTime;
		}
		if(curTag)
		{
			if(relx<(curTagX+curTagLen))
				curTagY=imin(curTagY,data[i].timing*height/maxTime);
			else
			{
				//Tag is before this sample
				font->Render(curTag->c_str() ,-1,FTPoint(curTagX,imax(curTagY-curTagH,0)));
				curTag=NULL;
			}
		}
	}
}

ParseThread::ParseThread(RootMovieClip* r,istream& in):f(in),isEnded(false),root(NULL),version(0),useAVM2(false)
{
	root=r;
	sem_init(&ended,0,0);
}

ParseThread::~ParseThread()
{
	sem_destroy(&ended);
}

void ParseThread::execute()
{
	pt=this;
	try
	{
		SWF_HEADER h(f);
		if(!h.valid)
			throw ParseException("Not an SWF file");
		root->setFrameSize(h.getFrameSize());
		root->setFrameCount(h.FrameCount);

		//Create a top level TagFactory
		TagFactory factory(f, true);
		bool done=false;
		bool empty=true;
		while(!done)
		{
			Tag* tag=factory.readTag();
			sys->tagsStorage.push_back(tag);
			switch(tag->getType())
			{
				case END_TAG:
				{
					LOG(LOG_NO_INFO,"End of parsing @ " << f.tellg());
					if(!empty)
						root->commitFrame(false);
					else
						root->revertFrame();
					done=true;
					root->check();
					break;
				}
				case DICT_TAG:
				{
					DictionaryTag* d=static_cast<DictionaryTag*>(tag);
					d->setLoadedFrom(root);
					root->addToDictionary(d);
					break;
				}
				case DISPLAY_LIST_TAG:
					root->addToFrame(static_cast<DisplayListTag*>(tag));
					empty=false;
					break;
				case SHOW_TAG:
					root->commitFrame(true);
					empty=true;
					break;
				case CONTROL_TAG:
					root->addToFrame(static_cast<ControlTag*>(tag));
					empty=false;
					break;
				case FRAMELABEL_TAG:
					root->labelCurrentFrame(static_cast<FrameLabelTag*>(tag)->Name);
					empty=false;
					break;
				case TAG:
					//Not yet implemented tag, ignore it
					break;
			}
			if(sys->shouldTerminate() || aborting)
				break;
		}
	}
	catch(LightsparkException& e)
	{
		LOG(LOG_ERROR,"Exception in ParseThread " << e.cause);
		root->parsingFailed();
		sys->setError(e.cause);
	}
	pt=NULL;

	sem_post(&ended);
}

void ParseThread::threadAbort()
{
	//Tell the our RootMovieClip that the parsing is ending
	root->parsingFailed();
}

void ParseThread::wait()
{
	if(!isEnded)
	{
		sem_wait(&ended);
		isEnded=true;
	}
}

void RenderThread::wait()
{
	if(terminated)
		return;
	terminated=true;
	//Signal potentially blocking semaphore
	sem_post(&render);
	int ret=pthread_join(t,NULL);
	assert_and_throw(ret==0);
}

InputThread::InputThread(SystemState* s,ENGINE e, void* param):m_sys(s),terminated(false),
	mutexListeners("Input listeners"),mutexDragged("Input dragged"),curDragged(NULL),lastMouseDownTarget(NULL)
{
	LOG(LOG_NO_INFO,"Creating input thread");
	if(e==SDL)
		pthread_create(&t,NULL,(thread_worker)sdl_worker,this);
#ifdef COMPILE_PLUGIN
	else if(e==GTKPLUG)
	{
		npapi_params=(NPAPI_params*)param;
		GtkWidget* container=npapi_params->container;
		gtk_widget_set_can_focus(container,True);
		gtk_widget_add_events(container,GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK |
						GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_EXPOSURE_MASK | GDK_VISIBILITY_NOTIFY_MASK |
						GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_FOCUS_CHANGE_MASK);
		g_signal_connect(G_OBJECT(container), "event", G_CALLBACK(gtkplug_worker), this);
	}
#endif
	else
		::abort();
}

InputThread::~InputThread()
{
	wait();
}

void InputThread::wait()
{
	if(terminated)
		return;
	pthread_join(t,NULL);
	terminated=true;
}

#ifdef COMPILE_PLUGIN
//This is a GTK event handler and the gdk lock is already acquired
gboolean InputThread::gtkplug_worker(GtkWidget *widget, GdkEvent *event, InputThread* th)
{
	//Set sys to this SystemState
	sys=th->m_sys;
	gboolean ret=FALSE;
	switch(event->type)
	{
		case GDK_KEY_PRESS:
		{
			//cout << "key press" << endl;
			switch(event->key.keyval)
			{
				case GDK_i:
					th->m_sys->showInteractiveMap=!th->m_sys->showInteractiveMap;
					break;
				case GDK_p:
					th->m_sys->showProfilingData=!th->m_sys->showProfilingData;
					break;
				default:
					break;
			}
			ret=TRUE;
			break;
		}
		case GDK_EXPOSE:
		{
			//Signal the renderThread
			th->m_sys->getRenderThread()->draw();
			ret=TRUE;
			break;
		}
		case GDK_BUTTON_PRESS:
		{
			//Grab focus
			gtk_widget_grab_focus(widget);
			//cout << "Press" << endl;
			Locker locker(th->mutexListeners);
			th->m_sys->getRenderThread()->requestInput();
			float selected=th->m_sys->getRenderThread()->getIdAt(event->button.x,event->button.y);
			if(selected!=0)
			{
				int index=lrint(th->listeners.size()*selected);
				index--;

				th->lastMouseDownTarget=th->listeners[index];
				//Add event to the event queue
				th->m_sys->currentVm->addEvent(th->listeners[index],Class<MouseEvent>::getInstanceS("mouseDown",true));
				//And select that object for debugging (if needed)
				if(th->m_sys->showDebug)
					th->m_sys->getRenderThread()->selectedDebug=th->listeners[index];
			}
			ret=TRUE;
			break;
		}
		case GDK_BUTTON_RELEASE:
		{
			//cout << "Release" << endl;
			Locker locker(th->mutexListeners);
			th->m_sys->getRenderThread()->requestInput();
			float selected=th->m_sys->getRenderThread()->getIdAt(event->button.x,event->button.y);
			if(selected!=0)
			{
				int index=lrint(th->listeners.size()*selected);
				index--;

				//Add event to the event queue
				getVm()->addEvent(th->listeners[index],Class<MouseEvent>::getInstanceS("mouseUp",true));
				//Also send the click event
				if(th->lastMouseDownTarget==th->listeners[index])
				{
					getVm()->addEvent(th->listeners[index],Class<MouseEvent>::getInstanceS("click",true));
					th->lastMouseDownTarget=NULL;
				}
			}
			ret=TRUE;
			break;
		}
		default:
#ifdef EXPENSIVE_DEBUG
			cout << "GDKTYPE " << event->type << endl;
#endif
			break;
	}
	return ret;
}
#endif

void* InputThread::sdl_worker(InputThread* th)
{
	sys=th->m_sys;
	SDL_Event event;
	while(SDL_WaitEvent(&event))
	{
		switch(event.type)
		{
			case SDL_KEYDOWN:
			{
				switch(event.key.keysym.sym)
				{
					case SDLK_d:
						th->m_sys->showDebug=!th->m_sys->showDebug;
						break;
					case SDLK_i:
						th->m_sys->showInteractiveMap=!th->m_sys->showInteractiveMap;
						break;
					case SDLK_p:
						th->m_sys->showProfilingData=!th->m_sys->showProfilingData;
						break;
					case SDLK_q:
						th->m_sys->setShutdownFlag();
						if(th->m_sys->currentVm)
							LOG(LOG_CALLS,"We still miss " << sys->currentVm->getEventQueueSize() << " events");
						pthread_exit(0);
						break;
					case SDLK_s:
						th->m_sys->state.stop_FP=true;
						break;
					case SDLK_DOWN:
						th->m_sys->yOffset-=10;
						break;
					case SDLK_UP:
						th->m_sys->yOffset+=10;
						break;
					case SDLK_LEFT:
						th->m_sys->xOffset-=10;
						break;
					case SDLK_RIGHT:
						th->m_sys->xOffset+=10;
						break;
					//Ignore any other keystrokes
					default:
						break;
				}
				break;
			}
			case SDL_MOUSEBUTTONDOWN:
			{
				Locker locker(th->mutexListeners);
				th->m_sys->getRenderThread()->requestInput();
				float selected=th->m_sys->getRenderThread()->getIdAt(event.button.x,event.button.y);
				if(selected==0)
				{
					th->m_sys->getRenderThread()->selectedDebug=NULL;
					break;
				}

				int index=lrint(th->listeners.size()*selected);
				index--;

				th->lastMouseDownTarget=th->listeners[index];
				//Add event to the event queue
				th->m_sys->currentVm->addEvent(th->listeners[index],Class<MouseEvent>::getInstanceS("mouseDown",true));
				//And select that object for debugging (if needed)
				if(th->m_sys->showDebug)
					th->m_sys->getRenderThread()->selectedDebug=th->listeners[index];
				break;
			}
			case SDL_MOUSEBUTTONUP:
			{
				Locker locker(th->mutexListeners);
				th->m_sys->getRenderThread()->requestInput();
				float selected=th->m_sys->getRenderThread()->getIdAt(event.button.x,event.button.y);
				if(selected==0)
					break;

				int index=lrint(th->listeners.size()*selected);
				index--;

				//Add event to the event queue
				getVm()->addEvent(th->listeners[index],Class<MouseEvent>::getInstanceS("mouseUp",true));
				//Also send the click event
				if(th->lastMouseDownTarget==th->listeners[index])
				{
					getVm()->addEvent(th->listeners[index],Class<MouseEvent>::getInstanceS("click",true));
					th->lastMouseDownTarget=NULL;
				}
				break;
			}
			case SDL_QUIT:
			{
				th->m_sys->setShutdownFlag();
				if(th->m_sys->currentVm)
					LOG(LOG_CALLS,"We still miss " << sys->currentVm->getEventQueueSize() << " events");
				pthread_exit(0);
				break;
			}
		}
	}
	return NULL;
}

void InputThread::addListener(InteractiveObject* ob)
{
	Locker locker(mutexListeners);
	assert(ob);

#ifndef NDEBUG
	vector<InteractiveObject*>::const_iterator it=find(listeners.begin(),listeners.end(),ob);
	//Object is already register, should not happen
	assert_and_throw(it==listeners.end());
#endif
	
	//Register the listener
	listeners.push_back(ob);
	unsigned int count=listeners.size();

	//Set a unique id for listeners in the range [0,1]
	//count is the number of listeners, this is correct so that no one gets 0
	float increment=1.0f/count;
	float cur=increment;
	for(unsigned int i=0;i<count;i++)
	{
		listeners[i]->setId(cur);
		cur+=increment;
	}
}

void InputThread::removeListener(InteractiveObject* ob)
{
	Locker locker(mutexListeners);

	vector<InteractiveObject*>::iterator it=find(listeners.begin(),listeners.end(),ob);
	if(it==listeners.end()) //Listener not found
		return;
	
	//Unregister the listener
	listeners.erase(it);
	
	unsigned int count=listeners.size();

	//Set a unique id for listeners in the range [0,1]
	//count is the number of listeners, this is correct so that no one gets 0
	float increment=1.0f/count;
	float cur=increment;
	for(unsigned int i=0;i<count;i++)
	{
		listeners[i]->setId(cur);
		cur+=increment;
	}
}

void InputThread::enableDrag(Sprite* s, const lightspark::RECT& limit)
{
	Locker locker(mutexDragged);
	if(s==curDragged)
		return;
	
	if(curDragged) //Stop dragging the previous sprite
		curDragged->decRef();
	
	assert(s);
	//We need to avoid that the object is destroyed
	s->incRef();
	
	curDragged=s;
	dragLimit=limit;
}

void InputThread::disableDrag()
{
	Locker locker(mutexDragged);
	if(curDragged)
	{
		curDragged->decRef();
		curDragged=NULL;
	}
}

RenderThread::RenderThread(SystemState* s,ENGINE e,void* params):m_sys(s),terminated(false),inputNeeded(false),inputDisabled(false),
	interactive_buffer(NULL),tempBufferAcquired(false),frameCount(0),secsCount(0),mutexResources("GLResource Mutex"),dataTex(false),
	mainTex(false),tempTex(false),inputTex(false),hasNPOTTextures(false),selectedDebug(NULL),currentId(0),materialOverride(false)
{
	LOG(LOG_NO_INFO,"RenderThread this=" << this);
	m_sys=s;
	sem_init(&render,0,0);
	sem_init(&inputDone,0,0);

#ifdef WIN32
	fontPath = "TimesNewRoman.ttf";
#else
	FcPattern *pat, *match;
	FcResult result = FcResultMatch;
	char *font = NULL;

	pat = FcPatternCreate();
	FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)"Serif");
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	match = FcFontMatch(NULL, pat, &result);
	FcPatternDestroy(pat);

	if (result != FcResultMatch)
	{
		LOG(LOG_ERROR,"Unable to find suitable Serif font");
		throw RunTimeException("Unable to find Serif font");
	}

	FcPatternGetString(match, FC_FILE, 0, (FcChar8 **) &font);
	fontPath = font;
	FcPatternDestroy(match);
	LOG(LOG_NO_INFO, "Font File is " << fontPath);
#endif

	if(e==SDL)
		pthread_create(&t,NULL,(thread_worker)sdl_worker,this);
#ifdef COMPILE_PLUGIN
	else if(e==GTKPLUG)
	{
		npapi_params=(NPAPI_params*)params;
		pthread_create(&t,NULL,(thread_worker)gtkplug_worker,this);
	}
#endif
	time_s = compat_get_current_time_ms();
}

RenderThread::~RenderThread()
{
	wait();
	sem_destroy(&render);
	sem_destroy(&inputDone);
	delete[] interactive_buffer;
	LOG(LOG_NO_INFO,"~RenderThread this=" << this);
}

void RenderThread::addResource(GLResource* res)
{
	managedResources.insert(res);
}

void RenderThread::removeResource(GLResource* res)
{
	managedResources.erase(res);
}

void RenderThread::acquireResourceMutex()
{
	mutexResources.lock();
}

void RenderThread::releaseResourceMutex()
{
	mutexResources.unlock();
}

void RenderThread::requestInput()
{
	inputNeeded=true;
	sem_post(&render);
	sem_wait(&inputDone);
}

bool RenderThread::glAcquireIdBuffer()
{
	if(inputDisabled)
		return false;
	//TODO: PERF: on the id buffer stuff are drawn more than once
	if(currentId!=0)
	{
		glDrawBuffer(GL_COLOR_ATTACHMENT2);
		materialOverride=true;
		FILLSTYLE::fixedColor(currentId,currentId,currentId);
		return true;
	}
	
	return false;
}

void RenderThread::glReleaseIdBuffer()
{
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	materialOverride=false;
}

void RenderThread::glAcquireTempBuffer(number_t xmin, number_t xmax, number_t ymin, number_t ymax)
{
	assert(tempBufferAcquired==false);
	tempBufferAcquired=true;

	glDrawBuffer(GL_COLOR_ATTACHMENT1);
	materialOverride=false;
	
	glDisable(GL_BLEND);
	glColor4f(0,0,0,0); //No output is fairly ok to clear
	glBegin(GL_QUADS);
		glVertex2f(xmin,ymin);
		glVertex2f(xmax,ymin);
		glVertex2f(xmax,ymax);
		glVertex2f(xmin,ymax);
	glEnd();
}

void RenderThread::glBlitTempBuffer(number_t xmin, number_t xmax, number_t ymin, number_t ymax)
{
	assert(tempBufferAcquired==true);
	tempBufferAcquired=false;

	//Use the blittler program to blit only the used buffer
	glUseProgram(blitter_program);
	glEnable(GL_BLEND);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	rt->tempTex.bind();
	glBegin(GL_QUADS);
		glVertex2f(xmin,ymin);
		glVertex2f(xmax,ymin);
		glVertex2f(xmax,ymax);
		glVertex2f(xmin,ymax);
	glEnd();
	glUseProgram(gpu_program);
}

#ifdef COMPILE_PLUGIN
void* RenderThread::gtkplug_worker(RenderThread* th)
{
	sys=th->m_sys;
	rt=th;
	NPAPI_params* p=th->npapi_params;

	RECT size=sys->getFrameSize();
	int swf_width=size.Xmax/20;
	int swf_height=size.Ymax/20;

	int window_width=p->width;
	int window_height=p->height;

	float scalex=window_width;
	scalex/=swf_width;
	float scaley=window_height;
	scaley/=swf_height;

	rt->width=window_width;
	rt->height=window_height;
	
	Display* d=XOpenDisplay(NULL);

	int a,b;
	Bool glx_present=glXQueryVersion(d,&a,&b);
	if(!glx_present)
	{
		LOG(LOG_ERROR,"glX not present");
		return NULL;
	}
	int attrib[10]={GLX_BUFFER_SIZE,24,GLX_DOUBLEBUFFER,True,None};
	GLXFBConfig* fb=glXChooseFBConfig(d, 0, attrib, &a);
	if(!fb)
	{
		attrib[2]=None;
		fb=glXChooseFBConfig(d, 0, attrib, &a);
		LOG(LOG_ERROR,"Falling back to no double buffering");
	}
	if(!fb)
	{
		LOG(LOG_ERROR,"Could not find any GLX configuration");
		::abort();
	}
	int i;
	for(i=0;i<a;i++)
	{
		int id;
		glXGetFBConfigAttrib(d,fb[i],GLX_VISUAL_ID,&id);
		if(id==(int)p->visual)
			break;
	}
	if(i==a)
	{
		//No suitable id found
		LOG(LOG_ERROR,"No suitable graphics configuration available");
		return NULL;
	}
	th->mFBConfig=fb[i];
	cout << "Chosen config " << hex << fb[i] << dec << endl;
	XFree(fb);

	th->mContext = glXCreateNewContext(d,th->mFBConfig,GLX_RGBA_TYPE ,NULL,1);
	GLXWindow glxWin=p->window;
	glXMakeCurrent(d, glxWin,th->mContext);
	if(!glXIsDirect(d,th->mContext))
		printf("Indirect!!\n");

	th->commonGLInit(window_width, window_height);
	
	ThreadProfile* profile=sys->allocateProfiler(RGB(200,0,0));
	profile->setTag("Render");
	FTTextureFont font(rt->fontPath.c_str());
	if(font.Error())
	{
		LOG(LOG_ERROR,"Unable to load serif font");
		throw RunTimeException("Unable to load font");
	}
	
	font.FaceSize(20);

	glEnable(GL_TEXTURE_2D);
	try
	{
		while(1)
		{
			sem_wait(&th->render);
			Chronometer chronometer;
			
			if(th->inputNeeded)
			{
				th->inputTex.bind();
				glGetTexImage(GL_TEXTURE_2D,0,GL_BGRA,GL_UNSIGNED_BYTE,th->interactive_buffer);
				th->inputNeeded=false;
				sem_post(&th->inputDone);
			}

			//Before starting rendering, cleanup all the request arrived in the meantime
			int fakeRenderCount=0;
			while(sem_trywait(&th->render)==0)
			{
				if(th->m_sys->isShuttingDown())
					break;
				fakeRenderCount++;
			}
			
			if(fakeRenderCount)
				LOG(LOG_NO_INFO,"Faking " << fakeRenderCount << " renderings");
			if(th->m_sys->isShuttingDown())
				break;

			if(th->m_sys->isOnError())
			{
				glUseProgram(0);

				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				glDrawBuffer(GL_BACK);
				glLoadIdentity();

				glClearColor(0,0,0,1);
				glClear(GL_COLOR_BUFFER_BIT);
				glColor3f(0.8,0.8,0.8);
					    
				font.Render("We're sorry, Lightspark encountered a yet unsupported Flash file",
					    -1,FTPoint(0,th->height/2));

				stringstream errorMsg;
				errorMsg << "SWF file: " << th->m_sys->getOrigin();
				font.Render(errorMsg.str().c_str(),
					    -1,FTPoint(0,th->height/2-20));
					    
				errorMsg.str("");
				errorMsg << "Cause: " << th->m_sys->errorCause;
				font.Render(errorMsg.str().c_str(),
					    -1,FTPoint(0,th->height/2-40));
				
				glFlush();
				glXSwapBuffers(d,glxWin);
			}
			else
			{
				glXSwapBuffers(d,glxWin);

				glBindFramebuffer(GL_FRAMEBUFFER, rt->fboId);
				glDrawBuffer(GL_COLOR_ATTACHMENT0);

				RGB bg=sys->getBackground();
				glClearColor(bg.Red/255.0F,bg.Green/255.0F,bg.Blue/255.0F,0);
				glClear(GL_COLOR_BUFFER_BIT);
				glLoadIdentity();
				glScalef(scalex,scaley,1);
				
				sys->Render();

				glFlush();

				glLoadIdentity();

				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				glDrawBuffer(GL_BACK);

				glClearColor(0,0,0,1);
				glClear(GL_COLOR_BUFFER_BIT);

				TextureBuffer* curBuf=((th->m_sys->showInteractiveMap)?&th->inputTex:&th->mainTex);
				curBuf->bind();
				curBuf->setTexScale(th->fragmentTexScaleUniform);
				glColor4f(0,0,1,0);
				glBegin(GL_QUADS);
					glTexCoord2f(0,1);
					glVertex2i(0,0);
					glTexCoord2f(1,1);
					glVertex2i(th->width,0);
					glTexCoord2f(1,0);
					glVertex2i(th->width,th->height);
					glTexCoord2f(0,0);
					glVertex2i(0,th->height);
				glEnd();

				if(sys->showProfilingData)
				{
					glUseProgram(0);
					glDisable(GL_TEXTURE_2D);

					//Draw bars
					glColor4f(0.7,0.7,0.7,0.7);
					glBegin(GL_LINES);
					for(int i=1;i<10;i++)
					{
						glVertex2i(0,(i*th->height/10));
						glVertex2i(th->width,(i*th->height/10));
					}
					glEnd();
				
					list<ThreadProfile>::iterator it=sys->profilingData.begin();
					for(;it!=sys->profilingData.end();it++)
						it->plot(1000000/sys->getFrameRate(),&font);

					glEnable(GL_TEXTURE_2D);
					glUseProgram(rt->gpu_program);
				}
				//Call glFlush to offload work on the GPU
				glFlush();
			}
			profile->accountTime(chronometer.checkpoint());
		}
	}
	catch(LightsparkException& e)
	{
		LOG(LOG_ERROR,"Exception in RenderThread " << e.what());
		sys->setError(e.cause);
	}
	glDisable(GL_TEXTURE_2D);
	//Before destroying the context shutdown all the GLResources
	set<GLResource*>::const_iterator it=th->managedResources.begin();
	for(;it!=th->managedResources.end();it++)
		(*it)->shutdown();
	th->commonGLDeinit();
	glXMakeCurrent(d,None,NULL);
	glXDestroyContext(d,th->mContext);
	XCloseDisplay(d);
	return NULL;
}
#endif

bool RenderThread::loadShaderPrograms()
{
	//Create render program
	assert(glCreateShader);
	GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
	
	const char *fs = NULL;
	fs = dataFileRead("lightspark.frag");
	if(fs==NULL)
	{
		LOG(LOG_ERROR,"Shader lightspark.frag not found");
		throw RunTimeException("Fragment shader code not found");
	}
	assert(glShaderSource);
	glShaderSource(f, 1, &fs,NULL);
	free((void*)fs);

	bool ret=true;
	char str[1024];
	int a;
	assert(glCompileShader);
	glCompileShader(f);
	assert(glGetShaderInfoLog);
	glGetShaderInfoLog(f,1024,&a,str);
	LOG(LOG_NO_INFO,"Fragment shader compilation " << str);

	assert(glCreateProgram);
	gpu_program = glCreateProgram();
	assert(glAttachShader);
	glAttachShader(gpu_program,f);

	assert(glLinkProgram);
	glLinkProgram(gpu_program);
	assert(glGetProgramiv);
	glGetProgramiv(gpu_program,GL_LINK_STATUS,&a);
	if(a==GL_FALSE)
	{
		ret=false;
		return ret;
	}
	
	//Create the blitter shader
	GLuint v = glCreateShader(GL_VERTEX_SHADER);

	fs = dataFileRead("lightspark.vert");
	if(fs==NULL)
	{
		LOG(LOG_ERROR,"Shader lightspark.vert not found");
		throw RunTimeException("Vertex shader code not found");
	}
	glShaderSource(v, 1, &fs,NULL);
	free((void*)fs);

	glCompileShader(v);
	glGetShaderInfoLog(v,1024,&a,str);
	LOG(LOG_NO_INFO,"Vertex shader compilation " << str);

	blitter_program = glCreateProgram();
	glAttachShader(blitter_program,v);
	
	glLinkProgram(blitter_program);
	glGetProgramiv(blitter_program,GL_LINK_STATUS,&a);
	if(a==GL_FALSE)
	{
		ret=false;
		return ret;
	}

	assert(ret);
	return true;
}

float RenderThread::getIdAt(int x, int y)
{
	//TODO: use floating point textures
	uint32_t allocWidth=inputTex.getAllocWidth();
	return (interactive_buffer[y*allocWidth+x]&0xff)/255.0f;
}

void RootMovieClip::initialize()
{
	if(!initialized && sys->currentVm)
	{
		initialized=true;
		//Let's see if we have to bind the root movie clip itself
		if(bindName.len())
			sys->currentVm->addEvent(NULL,new BindClassEvent(this,bindName));
		//Now signal the completion for this root
		sys->currentVm->addEvent(loaderInfo,Class<Event>::getInstanceS("init"));
		//Wait for handling of all previous events
		SynchronizationEvent* se=new SynchronizationEvent;
		bool added=sys->currentVm->addEvent(NULL, se);
		if(!added)
		{
			se->decRef();
			throw RunTimeException("Could not add event");
		}
		se->wait();
		se->decRef();
	}
}

bool RootMovieClip::getBounds(number_t& xmin, number_t& xmax, number_t& ymin, number_t& ymax) const
{
	RECT f=getFrameSize();
	xmin=0;
	ymin=0;
	xmax=f.Xmax;
	ymax=f.Ymax;
	return true;
}

void RootMovieClip::Render()
{
	Locker l(mutexFrames);
	while(1)
	{
		//Check if the next frame we are going to play is available
		if(state.next_FP<frames.size())
			break;

		l.unlock();
		sem_wait(&new_frame);
		if(parsingIsFailed)
			return;
		l.lock();
	}

	MovieClip::Render();
}

void RenderThread::commonGLDeinit()
{
	glBindFramebuffer(GL_FRAMEBUFFER,0);
	glDeleteFramebuffers(1,&rt->fboId);
	dataTex.shutdown();
	mainTex.shutdown();
	tempTex.shutdown();
	inputTex.shutdown();
}

void RenderThread::commonGLInit(int width, int height)
{
	//Now we can initialize GLEW
	glewExperimental = GL_TRUE;
	GLenum err = glewInit();
	if (GLEW_OK != err)
	{
		LOG(LOG_ERROR,"Cannot initialize GLEW");
		cout << glewGetErrorString(err) << endl;
		::abort();
	}
	if(!GLEW_VERSION_2_0)
	{
		LOG(LOG_ERROR,"Video card does not support OpenGL 2.0... Aborting");
		::abort();
	}
	if(GLEW_ARB_texture_non_power_of_two)
		hasNPOTTextures=true;

	//Load shaders
	loadShaderPrograms();

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);
	
	glViewport(0,0,width,height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0,width,0,height,-100,0);

	glMatrixMode(GL_MODELVIEW);
	glActiveTexture(GL_TEXTURE0);

	dataTex.init();

	mainTex.init(width, height, GL_NEAREST);

	tempTex.init(width, height, GL_NEAREST);

	inputTex.init(width, height, GL_NEAREST);
	//Allocated buffer for texture readback
	interactive_buffer=new uint32_t[inputTex.getAllocWidth()*inputTex.getAllocHeight()];

	//Set uniforms
	cleanGLErrors();
	glUseProgram(blitter_program);
	int texScale=glGetUniformLocation(blitter_program,"texScale");
	mainTex.setTexScale(texScale);
	cleanGLErrors();

	glUseProgram(gpu_program);
	cleanGLErrors();
	int tex=glGetUniformLocation(gpu_program,"g_tex1");
	glUniform1i(tex,0);
	fragmentTexScaleUniform=glGetUniformLocation(gpu_program,"texScale");
	glUniform2f(fragmentTexScaleUniform,1,1);
	cleanGLErrors();

	//Default to replace
	glTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
	// create a framebuffer object
	glGenFramebuffers(1, &fboId);
	glBindFramebuffer(GL_FRAMEBUFFER, fboId);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D, mainTex.getId(), 0);
	//Verify if we have more than an attachment available (1 is guaranteed)
	GLint numberOfAttachments=0;
	glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &numberOfAttachments);
	if(numberOfAttachments>=3)
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,GL_TEXTURE_2D, tempTex.getId(), 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2,GL_TEXTURE_2D, inputTex.getId(), 0);
	}
	else
	{
		LOG(LOG_ERROR,"Non enough color attachments available, input disabled");
		inputDisabled=true;
	}
	
	// check FBO status
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if(status != GL_FRAMEBUFFER_COMPLETE)
	{
		LOG(LOG_ERROR,"Incomplete FBO status " << status << "... Aborting");
		while(err!=GL_NO_ERROR)
		{
			LOG(LOG_ERROR,"GL errors during initialization: " << err);
			err=glGetError();
		}
		::abort();
	}
}

void* RenderThread::sdl_worker(RenderThread* th)
{
	sys=th->m_sys;
	rt=th;
	RECT size=sys->getFrameSize();
	int width=size.Xmax/20;
	int height=size.Ymax/20;
	rt->width=width;
	rt->height=height;
	SDL_GL_SetAttribute( SDL_GL_RED_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_SWAP_CONTROL, 0 );
	SDL_GL_SetAttribute( SDL_GL_ACCELERATED_VISUAL, 1); 
	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );

	SDL_SetVideoMode( width, height, 24, SDL_OPENGL );
	th->commonGLInit(width, height);

	ThreadProfile* profile=sys->allocateProfiler(RGB(200,0,0));
	profile->setTag("Render");
	FTTextureFont font(rt->fontPath.c_str());
	if(font.Error())
		throw RunTimeException("Unable to load font");
	
	font.FaceSize(20);
	try
	{
		//Texturing must be enabled otherwise no tex coord will be sent to the shader
		glEnable(GL_TEXTURE_2D);
		Chronometer chronometer;
		while(1)
		{
			sem_wait(&th->render);
			chronometer.checkpoint();

			SDL_GL_SwapBuffers( );

			if(th->inputNeeded)
			{
				th->inputTex.bind();
				glGetTexImage(GL_TEXTURE_2D,0,GL_BGRA,GL_UNSIGNED_BYTE,th->interactive_buffer);
				th->inputNeeded=false;
				sem_post(&th->inputDone);
			}

			//Before starting rendering, cleanup all the request arrived in the meantime
			int fakeRenderCount=0;
			while(sem_trywait(&th->render)==0)
			{
				if(th->m_sys->isShuttingDown())
					break;
				fakeRenderCount++;
			}

			if(fakeRenderCount)
				LOG(LOG_NO_INFO,"Faking " << fakeRenderCount << " renderings");

			if(th->m_sys->isShuttingDown())
				break;
			SDL_PumpEvents();

			if(th->m_sys->isOnError())
			{
				glUseProgram(0);

				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				glDrawBuffer(GL_BACK);
				glLoadIdentity();

				glClearColor(0,0,0,1);
				glClear(GL_COLOR_BUFFER_BIT);
				glColor3f(0.8,0.8,0.8);
					    
				font.Render("We're sorry, Lightspark encountered a yet unsupported Flash file",
						-1,FTPoint(0,th->height/2));

				stringstream errorMsg;
				errorMsg << "SWF file: " << th->m_sys->getOrigin();
				font.Render(errorMsg.str().c_str(),
						-1,FTPoint(0,th->height/2-20));
					    
				errorMsg.str("");
				errorMsg << "Cause: " << th->m_sys->errorCause;
				font.Render(errorMsg.str().c_str(),
						-1,FTPoint(0,th->height/2-40));

				font.Render("Press 'Q' to exit",-1,FTPoint(0,th->height/2-60));
				
				glFlush();
				SDL_GL_SwapBuffers( );
			}
			else
			{
				glBindFramebuffer(GL_FRAMEBUFFER, rt->fboId);
				
				//Clear the id buffer
				glDrawBuffer(GL_COLOR_ATTACHMENT2);
				glClearColor(0,0,0,0);
				glClear(GL_COLOR_BUFFER_BIT);
				
				//Clear the back buffer
				glDrawBuffer(GL_COLOR_ATTACHMENT0);
				RGB bg=sys->getBackground();
				glClearColor(bg.Red/255.0F,bg.Green/255.0F,bg.Blue/255.0F,1);
				glClear(GL_COLOR_BUFFER_BIT);
				
				glLoadIdentity();
				glTranslatef(th->m_sys->xOffset,th->m_sys->yOffset,0);
				
				th->m_sys->Render();

				glFlush();

				glLoadIdentity();

				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				glDrawBuffer(GL_BACK);
				glDisable(GL_BLEND);

				TextureBuffer* curBuf=((th->m_sys->showInteractiveMap)?&th->inputTex:&th->mainTex);
				curBuf->bind();
				curBuf->setTexScale(th->fragmentTexScaleUniform);
				glColor4f(0,0,1,0);
				glBegin(GL_QUADS);
					glTexCoord2f(0,1);
					glVertex2i(0,0);
					glTexCoord2f(1,1);
					glVertex2i(width,0);
					glTexCoord2f(1,0);
					glVertex2i(width,height);
					glTexCoord2f(0,0);
					glVertex2i(0,height);
				glEnd();
				
				if(th->m_sys->showDebug)
				{
					glUseProgram(0);
					glDisable(GL_TEXTURE_2D);
					if(th->selectedDebug)
						th->selectedDebug->debugRender(&font, true);
					else
						th->m_sys->debugRender(&font, true);
					glEnable(GL_TEXTURE_2D);
				}

				if(th->m_sys->showProfilingData)
				{
					glUseProgram(0);
					glColor3f(0,0,0);
					char frameBuf[20];
					snprintf(frameBuf,20,"Frame %u",th->m_sys->state.FP);
					font.Render(frameBuf,-1,FTPoint(0,0));

					//Draw bars
					glColor4f(0.7,0.7,0.7,0.7);
					glBegin(GL_LINES);
					for(int i=1;i<10;i++)
					{
						glVertex2i(0,(i*height/10));
						glVertex2i(width,(i*height/10));
					}
					glEnd();
					
					list<ThreadProfile>::iterator it=th->m_sys->profilingData.begin();
					for(;it!=th->m_sys->profilingData.end();it++)
						it->plot(1000000/sys->getFrameRate(),&font);
				}
				//Call glFlush to offload work on the GPU
				glFlush();
				glUseProgram(th->gpu_program);
				glEnable(GL_BLEND);
			}
			profile->accountTime(chronometer.checkpoint());
		}
		glDisable(GL_TEXTURE_2D);
	}
	catch(LightsparkException& e)
	{
		LOG(LOG_ERROR,"Exception in RenderThread " << e.cause);
		sys->setError(e.cause);
	}
	th->commonGLDeinit();
	return NULL;
}

void RenderThread::draw()
{
	sem_post(&render);
	time_d = compat_get_current_time_ms();
	uint64_t diff=time_d-time_s;
	if(diff>1000)
	{
		time_s=time_d;
		LOG(LOG_NO_INFO,"FPS: " << dec << frameCount);
		frameCount=0;
		secsCount++;
	}
	else
		frameCount++;
}

void RenderThread::tick()
{
	draw();
}

void RootMovieClip::setFrameCount(int f)
{
	Locker l(mutexFrames);
	totalFrames=f;
	state.max_FP=f;
	//TODO, maybe the next is a regular assert
	assert_and_throw(cur_frame==&frames.back());
	//Reserving guarantees than the vector is never invalidated
	frames.reserve(f);
	cur_frame=&frames.back();
}

void RootMovieClip::setFrameSize(const lightspark::RECT& f)
{
	frameSize=f;
	assert_and_throw(f.Xmin==0 && f.Ymin==0);
	sem_post(&sem_valid_size);
}

lightspark::RECT RootMovieClip::getFrameSize() const
{
	//This is a sync semaphore the first time and then a mutex
	sem_wait(&sem_valid_size);
	lightspark::RECT ret=frameSize;
	sem_post(&sem_valid_size);
	return ret;
}

void RootMovieClip::setFrameRate(float f)
{
	frameRate=f;
	sem_post(&sem_valid_rate);
}

float RootMovieClip::getFrameRate() const
{
	//This is a sync semaphore the first time and then a mutex
	sem_wait(&sem_valid_rate);
	float ret=frameRate;
	sem_post(&sem_valid_rate);
	return ret;
}

void RootMovieClip::addToDictionary(DictionaryTag* r)
{
	sem_wait(&mutex);
	dictionary.push_back(r);
	sem_post(&mutex);
}

void RootMovieClip::addToFrame(DisplayListTag* t)
{
	sem_wait(&mutex);
	MovieClip::addToFrame(t);
	sem_post(&mutex);
}

void RootMovieClip::labelCurrentFrame(const STRING& name)
{
	Locker l(mutexFrames);
	frames.back().Label=(const char*)name;
}

void RootMovieClip::addToFrame(ControlTag* t)
{
	cur_frame->controls.push_back(t);
}

void RootMovieClip::commitFrame(bool another)
{
	Locker l(mutexFrames);
	framesLoaded=frames.size();
	if(another)
	{
		frames.push_back(Frame());
		cur_frame=&frames.back();
	}
	else
		cur_frame=NULL;

	assert_and_throw(frames.size()<=frames.capacity());

	if(framesLoaded==1)
	{
		//Let's initialize the first frame of this movieclip
		bootstrap();
		//TODO Should dispatch INIT here
		//Root movie clips are initialized now, after the first frame is really ready 
		initialize();
		//Now the bindings are effective

		//When the first frame is committed the frame rate is known
		sys->addTick(1000/frameRate,this);
	}
	sem_post(&new_frame);
}

void RootMovieClip::revertFrame()
{
	Locker l(mutexFrames);
	//TODO: The next should be a regular assert
	assert_and_throw(frames.size() && framesLoaded==(frames.size()-1));
	frames.pop_back();
	cur_frame=NULL;
}

RGB RootMovieClip::getBackground()
{
	return Background;
}

void RootMovieClip::setBackground(const RGB& bg)
{
	Background=bg;
}

DictionaryTag* RootMovieClip::dictionaryLookup(int id)
{
	sem_wait(&mutex);
	list< DictionaryTag*>::iterator it = dictionary.begin();
	for(;it!=dictionary.end();it++)
	{
		if((*it)->getId()==id)
			break;
	}
	if(it==dictionary.end())
	{
		LOG(LOG_ERROR,"No such Id on dictionary " << id);
		sem_post(&mutex);
		throw RunTimeException("Could not find an object on the dictionary");
	}
	DictionaryTag* ret=*it;
	sem_post(&mutex);
	return ret;
}

void RootMovieClip::tick()
{
	//Frame advancement may cause exceptions
	try
	{
		advanceFrame();
		Event* e=Class<Event>::getInstanceS("enterFrame");
		if(hasEventListener("enterFrame"))
			getVm()->addEvent(this,e);
		//Get a copy of the current childs
		vector<MovieClip*> curChildren;
		{
			Locker l(mutexChildrenClips);
			curChildren.reserve(childrenClips.size());
			curChildren.insert(curChildren.end(),childrenClips.begin(),childrenClips.end());
			for(uint32_t i=0;i<curChildren.size();i++)
				curChildren[i]->incRef();
		}
		//Advance all the children, and release the reference
		for(uint32_t i=0;i<curChildren.size();i++)
		{
			curChildren[i]->advanceFrame();
			if(curChildren[i]->hasEventListener("enterFrame"))
				getVm()->addEvent(curChildren[i],e);
			curChildren[i]->decRef();
		}
		e->decRef();
	}
	catch(LightsparkException& e)
	{
		LOG(LOG_ERROR,"Exception in RootMovieClip::tick " << e.cause);
		sys->setError(e.cause);
	}
}

/*ASObject* RootMovieClip::getVariableByQName(const tiny_string& name, const tiny_string& ns, ASObject*& owner)
{
	sem_wait(&mutex);
	ASObject* ret=ASObject::getVariableByQName(name, ns, owner);
	sem_post(&mutex);
	return ret;
}

void RootMovieClip::setVariableByMultiname(multiname& name, ASObject* o)
{
	sem_wait(&mutex);
	ASObject::setVariableByMultiname(name,o);
	sem_post(&mutex);
}

void RootMovieClip::setVariableByQName(const tiny_string& name, const tiny_string& ns, ASObject* o)
{
	sem_wait(&mutex);
	ASObject::setVariableByQName(name,ns,o);
	sem_post(&mutex);
}

void RootMovieClip::setVariableByString(const string& s, ASObject* o)
{
	abort();
	//TODO: ActionScript2 support
	string sub;
	int f=0;
	int l=0;
	ASObject* target=this;
	for(l;l<s.size();l++)
	{
		if(s[l]=='.')
		{
			sub=s.substr(f,l-f);
			ASObject* next_target;
			if(f==0 && sub=="__Packages")
			{
				next_target=&sys->cur_render_thread->vm.Global;
				owner=&sys->cur_render_thread->vm.Global;
			}
			else
				next_target=target->getVariableByQName(sub.c_str(),"",owner);

			f=l+1;
			if(!owner)
			{
				next_target=new Package;
				target->setVariableByQName(sub.c_str(),"",next_target);
			}
			target=next_target;
		}
	}
	sub=s.substr(f,l-f);
	target->setVariableByQName(sub.c_str(),"",o);
}*/


