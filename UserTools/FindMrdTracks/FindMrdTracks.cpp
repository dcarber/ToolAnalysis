/* vim:set noexpandtab tabstop=4 wrap */
#include "FindMrdTracks.h"
#include "TCanvas.h"
#include <numeric>      // std::iota

FindMrdTracks::FindMrdTracks():Tool(){}

bool FindMrdTracks::Initialise(std::string configfile, DataModel &data){
	
	if(verbose) cout<<"Initializing tool FindMrdTracks"<<endl;
	
	/////////////////// Useful header ///////////////////////
	if(configfile!="") m_variables.Initialise(configfile); //loading config file
	//m_variables.Print();
	
	m_data= &data; //assigning transient data pointer
	/////////////////////////////////////////////////////////////////
	
	// get configuration variables for this tool
	m_variables.Get("OutputDirectory",outputdir);
	m_variables.Get("verbose",verbose);
	verbose=10;
	m_variables.Get("MinDigitsForTrack",minimumdigits);
	m_variables.Get("MaxMrdSubEventDuration",maxsubeventduration);
	m_variables.Get("WriteTracksToFile",writefile);
	
	// create a BoostStore for recording the found tracks
	m_data->Stores["MRDSubEvents"] = new BoostStore(2,false);
	
	m_data->Stores["ANNIEEvent"]->Header->Get("AnnieGeometry",geo);
	numvetopmts = geo->GetNumVetoPMTs();
	
	// create clonesarray for storing the MRD Track details as they're found
	if(SubEventArray==nullptr) SubEventArray = new TClonesArray("cMRDSubEvent");  // string is class name
	
	return true;
}


bool FindMrdTracks::Execute(){
	
	if(verbose) cout<<"Tool FindMrdTracks finding tracks in next event."<<endl;
	
	int lastrunnum=runnum;
	int lastsubrunnum=subrunnum;
	
	// Get the ANNIE event and extract information
	//cout<<"FindMrdTracks getting event info from store"<<endl;
	m_data->Stores["ANNIEEvent"]->Get("RunNumber",runnum);
	m_data->Stores["ANNIEEvent"]->Get("SubrunNumber",subrunnum);
	m_data->Stores["ANNIEEvent"]->Get("MCFile",currentfilestring);
	m_data->Stores["ANNIEEvent"]->Get("EventNumber",eventnum);
	m_data->Stores["ANNIEEvent"]->Get("TriggerNumber",triggernum);
	m_data->Stores["ANNIEEvent"]->Get("TDCData",TDCData);  // TDCData is a std::map<ChannelKey,vector<TDCHit>>
	//cout<<"gotit"<<endl;
	
	// make the tree if we want to save the Tracks to file
	if( writefile && (mrdtrackfile==nullptr || (lastrunnum!=runnum) || (lastsubrunnum!=subrunnum)) ){
		//cout<<"creating new file to write MRD tracks to"<<endl;
		StartNewFile();  // open a new file for each run / subrun
	}
	
	// extract the digits from the annieevent and put them into separate vectors used by the track finder
	mrddigittimesthisevent.clear();
	mrddigitpmtsthisevent.clear();
	mrddigitchargesthisevent.clear();
	if(!TDCData){ /*cout<<"no TDC data to find MRD tracks in"<<endl;*/ return true; }
	if(TDCData->size()==0){ /*cout<<"no TDC hits to find tracks in "<<endl;*/ return true; }
	//cout<<"retrieving digit info from "<<TDCData->size()<<" hit pmts"<<endl;
	for(auto&& anmrdpmt : (*TDCData)){
		// retrieve the digit information
		// ============================
		//WCSimRootCherenkovDigiHit* digihit = 
		//	(WCSimRootCherenkovDigiHit*)atrigm->GetCherenkovDigiHits()->At(i);
		/*
		digihit is of type std::pair<ChannelKey,vector<TDCHit>>, 
		ChannelKey has members SubDetectorIndex (uint) and DetectorElementIndex (uint)
		TDCHit has members Time (type Timeclass), 
		*/
		
		ChannelKey chankey = anmrdpmt.first;
		// subdetector thispmtssubdetector = chankey.GetSubDetectorType(); will be TDC by nature
		int thispmtsid = chankey.GetDetectorElementIndex();
		if(thispmtsid < numvetopmts) continue; // this is a veto hit, not an MRD hit.
		for(auto&& hitsonthismrdpmt : anmrdpmt.second){
			mrddigitpmtsthisevent.push_back(thispmtsid);
			mrddigittimesthisevent.push_back(hitsonthismrdpmt.GetTime().GetNs());
			mrddigitchargesthisevent.push_back(hitsonthismrdpmt.GetCharge());
		}
	}
	int numdigits = mrddigittimesthisevent.size();
	
	///////////////////////////
	// now do the track finding
	
	if(verbose){
		cout<<"Searching for MRD tracks in event "<<eventnum<<endl;
		cout<<"mrddigittimesthisevent.size()="<<numdigits<<endl;
	}
	SubEventArray->Clear("C");
/* 
if your class contains pointers, use TrackArray.Clear("C"). You MUST then provide a Clear() method in your class that properly performs clearing and memory freeing. (or "implements the reset procedure for pointer objects")
 see https://root.cern.ch/doc/master/classTClonesArray.html#a025645e1e80ea79b43a08536c763cae2
*/
	int mrdeventcounter=0;
	if(numdigits<minimumdigits){
		// =====================================================================================
		// NO DIGITS IN THIS EVENT
		// ======================================
		//new((*SubEventArray)[0]) cMRDSubEvent();
		nummrdsubeventsthisevent=mrdeventcounter;
		nummrdtracksthisevent=0;
		if(writefile){
			nummrdsubeventsthiseventb->Fill();
			nummrdtracksthiseventb->Fill();
			subeventsinthiseventb->Fill();
			//mrdtree->Fill();						// fill the branches so the entries align.
			mrdtrackfile->cd();
			mrdtree->SetEntries(nummrdtracksthiseventb->GetEntries());
			mrdtree->Write("",TObject::kOverwrite);
			gROOT->cd();
		}
		return true;
		// skip remainder
		// ======================================================================================
	}
	
	// MEASURE EVENT DURATION TO DETERMINE IF THERE IS MORE THAN ONE MRD SUB EVENT
	// ===========================================================================
	// first check: are all hits within a 30ns window (maxsubeventduration) If so, just one subevent. 
	double eventendtime = *std::max_element(mrddigittimesthisevent.begin(),mrddigittimesthisevent.end());
	double eventstarttime = *std::min_element(mrddigittimesthisevent.begin(),mrddigittimesthisevent.end());
	double eventduration = (eventendtime - eventstarttime);
	if(verbose>2){
		cout<<"mrd event start: "<<eventstarttime<<", end : "
		    <<eventendtime<<", duration : "<<eventduration<<endl;
	}
	
	if((eventduration<maxsubeventduration)&&(numdigits>=minimumdigits)){
	// JUST ONE SUBEVENT
	// =================
		if(verbose>2){
			cout<<"all hits this event within one subevent."<<endl;
		}
		std::vector<int> digitidsinasubevent(numdigits);
		std::iota(digitidsinasubevent.begin(),digitidsinasubevent.end(),1);
		// these aren't recorded for true data. Could be obtained for MC, but not essential for now
		std::vector<double> digitqsinasubevent(numdigits,0);
		std::vector<int> digitnumtruephots(numdigits,0);
		std::vector<int> particleidsinasubevent(numdigits,0);
		std::vector<double> photontimesinasubevent(numdigits,0);
		
		// used to pull information from the WCSim trigger on true photons, parents, charges etc.
		// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//		// loop over digits and extract info
//		for(int thisdigit=0;thisdigit<numdigits;thisdigit++){
//			int thisdigitsq = thedigihit->GetQ();
//			digitqsinasubevent.push_back(thisdigitsq);
//			// add all the unique parent ID's for digits contributing to this subevent (truth level info)
			// XXX we need this to be able to draw True tracks by true parentid! XXX
			// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//			std::vector<int> truephotonindices = thedigihit->GetPhotonIds();
//			digitnumtruephots.push_back(truephotonindices.size());
//			for(int truephoton=0; truephoton<truephotonindices.size(); truephoton++){
//				int thephotonsid = truephotonindices.at(truephoton);
//				WCSimRootCherenkovHitTime *thehittimeobject = (WCSimRootCherenkovHitTime*)atrigm->GetCherenkovHitTimes()->At(thephotonsid);
//				int thephotonsparentsubevent = thehittimeobject->GetParentID();
//				particleidsinasubevent.push_back(thephotonsparentsubevent);
//				double thephotonstruetime = thehittimeobject->GetTruetime();
//				photontimesinasubevent.push_back(thephotonstruetime);
//			}
//		}
		
		// used to pull information from the WCSim trigger on true tracks in the event. etc
		// XXX we need this to be able to draw True tracks by start / endpoints! XXX
		// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// we store the truth tracks in each cMRDSubEvent, so we need to pass the ones within the subevent
//		// time window to the constructor
//		int numtracks = atrigt->GetNtrack();
//		std::vector<WCSimRootTrack*> truetrackpointers;               // removed from cMRDSubEvent
		std::vector<std::pair<TVector3,TVector3>> truetrackvertices;  // replacement of above
		std::vector<Int_t> truetrackpdgs;                             // replacement of above
//		for(int truetracki=0; truetracki<numtracks; truetracki++){
//			WCSimRootTrack* nextrack = (WCSimRootTrack*)atrigt->GetTracks()->At(truetracki);
//			if((nextrack->GetFlag()==0)/*&&(nextrack->GetIpnu()!=11)*/) truetrackpointers.push_back(nextrack);
//		}
		// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		
		// construct the subevent from all the digits
		if(verbose){
			cout<<"constructing a single subevent for this event"<<endl;
		}
		//cout<<"before creating new cMRDSubEvent at [0] SubEventArray="<<SubEventArray<<endl;
		cMRDSubEvent* currentsubevent = new((*SubEventArray)[0]) cMRDSubEvent(0, currentfilestring, runnum, eventnum, triggernum, digitidsinasubevent, mrddigitpmtsthisevent, digitqsinasubevent, mrddigittimesthisevent, digitnumtruephots, photontimesinasubevent, particleidsinasubevent, truetrackvertices, truetrackpdgs);
		mrdeventcounter++;
		// can also use 'cMRDSubEvent* = (cMRDSubEvent*)SubEventArray.ConstructedAt(0);' followed by a bunch of
		// 'Set' calls to set all relevant fields. This bypasses the constructor, calling it only when 
		// necessary, saving time. In that case, we do not need to call SubEventArray.Clear();
		
		int tracksthissubevent=currentsubevent->GetTracks()->size();
		if(verbose){
			cout<<"the only subevent this event found "<<tracksthissubevent<<" tracks"<<endl;
		}
		nummrdsubeventsthisevent=1;
		nummrdtracksthisevent=tracksthissubevent;
		if(writefile){
			nummrdsubeventsthiseventb->Fill();
			nummrdtracksthiseventb->Fill();
			subeventsinthiseventb->Fill();
		}
		
	} else {
	// MORE THAN ONE MRD SUBEVENT
	// ===========================
		std::vector<int> digitidsinasubevent;
		std::vector<int> tubeidsinasubevent;
		std::vector<double> digitqsinasubevent;
		std::vector<double> digittimesinasubevent;
		std::vector<int> digitnumtruephots;
		std::vector<int> particleidsinasubevent;
		std::vector<double> photontimesinasubevent;
		
		// this event has multiple subevents. Need to split hits into which subevent they belong to.
		// scan over the times and look for gaps where no digits lie, using these to delimit 'subevents'
		std::vector<float> subeventhittimesv;   // a vector of the starting times of a given subevent
		std::vector<double> sorteddigittimes(mrddigittimesthisevent);
		std::sort(sorteddigittimes.begin(), sorteddigittimes.end());
		subeventhittimesv.push_back(sorteddigittimes.at(0));
		for(int i=0;i<sorteddigittimes.size()-1;i++){
			float timetonextdigit = sorteddigittimes.at(i+1)-sorteddigittimes.at(i);
			if(timetonextdigit>maxsubeventduration){
				subeventhittimesv.push_back(sorteddigittimes.at(i+1));
				if(verbose){
					cout<<"Setting subevent time threshold at "<<subeventhittimesv.back()<<endl;
//					cout<<"this digit is at "<<sorteddigittimes.at(i)<<endl;
//					cout<<"next digit is at "<<sorteddigittimes.at(i+1)<<endl;
//					try{
//						cout<<"next next digit is at "<<sorteddigittimes.at(i+2)<<endl;
//					} catch (...) { int i=0; }
				}
			}
		}
		if(verbose){
			cout<<subeventhittimesv.size()<<" subevents this event"<<endl;
		}
		
		// a vector to record the subevent number for each hit, to know if we've allocated it yet.
		std::vector<int> subeventnumthisevent(numdigits,-1);
		// another for true tracks
		// used to pull information from the WCSim trigger on true tracks in the event. etc
		// XXX we need this to be able to draw True tracks by start / endpoints! XXX
		// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		//int numtracks = atrigt->GetNtrack();
		//std::vector<int> subeventnumthisevent2(numtracks,-1);
		// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		
		// now we need to sort the digits into the subevents they belong to:
		// loop over subevents
		int mrdtrackcounter=0;   // not all the subevents will have a track
		for(int thissubevent=0; thissubevent<subeventhittimesv.size(); thissubevent++){
			if(verbose>2){
				cout<<"Digits in MRD at = "<<subeventhittimesv.at(thissubevent)<<"ns in event "<<eventnum<<endl;
			}
			// don't need to worry about lower bound as we start from lowest t peak and 
			// exclude already allocated hits
			
			float endtime = (thissubevent<(subeventhittimesv.size()-1)) ? 
				subeventhittimesv.at(thissubevent+1) : (eventendtime+1.);
			if(verbose>2){
				cout<<"endtime for subevent "<<thissubevent<<" is "<<endtime<<endl;
			}
			// times are not ordered, so scan through all digits for each subevent
			for(int thisdigit=0;thisdigit<numdigits;thisdigit++){
				if(subeventnumthisevent.at(thisdigit)<0 && mrddigittimesthisevent.at(thisdigit)< endtime ){
					// thisdigit is in thissubevent
					if(verbose>3){
						cout<<"adding digit at "<<mrddigittimesthisevent.at(thisdigit)<<" to subevent "<<thissubevent<<endl;
					}
					digitidsinasubevent.push_back(thisdigit);
					subeventnumthisevent.at(thisdigit)=thissubevent;
					tubeidsinasubevent.push_back(mrddigitpmtsthisevent.at(thisdigit));
					digittimesinasubevent.push_back(mrddigittimesthisevent.at(thisdigit));
					digitqsinasubevent.push_back(mrddigitchargesthisevent.at(thisdigit));
//					// add all the unique parent ID's for digits contributing to this subevent (truth level info)
					// XXX we need this to be able to draw True tracks by true parentid! XXX
					// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//					std::vector<int> truephotonindices = thedigihit->GetPhotonIds();
//					digitnumtruephots.push_back(truephotonindices.size());
//					for(int truephoton=0; truephoton<truephotonindices.size(); truephoton++){
//						int thephotonsid = truephotonindices.at(truephoton);
//						WCSimRootCherenkovHitTime *thehittimeobject = (WCSimRootCherenkovHitTime*)atrigm->GetCherenkovHitTimes()->At(thephotonsid);
//						int thephotonsparentsubevent = thehittimeobject->GetParentID();
//						particleidsinasubevent.push_back(thephotonsparentsubevent);
//						double thephotonstruetime = thehittimeobject->GetTruetime();
//						photontimesinasubevent.push_back(thephotonstruetime);
//					}
					// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
				}
			}
			digitnumtruephots.assign(digittimesinasubevent.size(),0);      // FIXME replacement of above
			photontimesinasubevent.assign(digittimesinasubevent.size(),0); // FIXME replacement of above
			particleidsinasubevent.assign(digittimesinasubevent.size(),0); // FIXME replacement of above
			
			// construct the subevent from all the digits
			if(digitidsinasubevent.size()>=minimumdigits){	// must have enough for a subevent
				// first scan through all truth tracks to find those within this subevent time window
				// used to pull information from the WCSim trigger on true tracks in the event. etc
				// XXX we need this to be able to draw True tracks! XXX
				// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
				//std::vector<WCSimRootTrack*> truetrackpointers;               // removed from cMRDSubEvent
				std::vector<std::pair<TVector3,TVector3>> truetrackvertices;    // replacement of above
				std::vector<Int_t> truetrackpdgs;                               // replacement of above
				//for(int truetracki=0; truetracki<numtracks; truetracki++){
				//	WCSimRootTrack* nextrack = (WCSimRootTrack*)atrigt->GetTracks()->At(truetracki);
				//	if((subeventnumthisevent2.at(truetracki)<0)&&(nextrack->GetTime()<endtime)
				//		&&(nextrack->GetFlag()==0)&&(nextrack->GetIpnu()!=11)){
				//		truetrackpointers.push_back(nextrack);
				//		subeventnumthisevent2.at(truetracki)=thissubevent;
				//	}
				//}
				// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
				
				if(verbose>3){
					cout<<"constructing subevent "<<mrdeventcounter<<" with "<<digitidsinasubevent.size()<<" digits"<<endl;
				}
				//cout<<"before creating new cMRDSubEvent at [mrdeventcounter] SubEventArray="<<SubEventArray<<endl;
				cMRDSubEvent* currentsubevent = new((*SubEventArray)[mrdeventcounter]) cMRDSubEvent(mrdeventcounter, currentfilestring, runnum, eventnum, triggernum, digitidsinasubevent, tubeidsinasubevent, digitqsinasubevent, digittimesinasubevent, digitnumtruephots, photontimesinasubevent, particleidsinasubevent, truetrackvertices, truetrackpdgs);
				mrdeventcounter++;
				mrdtrackcounter+=currentsubevent->GetTracks()->size();
				if(verbose>2){
					cout<<"subevent "<<thissubevent<<" found "<<currentsubevent->GetTracks()->size()<<" tracks"<<endl;
				}
			}
			
			// clear the vectors and loop to the next subevent
			digitidsinasubevent.clear();
			tubeidsinasubevent.clear();
			digitqsinasubevent.clear();
			digittimesinasubevent.clear();
			particleidsinasubevent.clear();
			photontimesinasubevent.clear();
			digitnumtruephots.clear();
			
		}
		
		// quick scan to check for any unallocated hits
		for(int k=0;k<subeventnumthisevent.size();k++){
			if(subeventnumthisevent.at(k)==-1 && verbose>2 ){cout<<"*****unbinned hit!"<<k<<" "<<mrddigittimesthisevent.at(k)<<endl;}
		}
		
		nummrdsubeventsthisevent=mrdeventcounter;
		nummrdtracksthisevent=mrdtrackcounter;
		if(writefile){
			nummrdsubeventsthiseventb->Fill();
			nummrdtracksthiseventb->Fill();
			subeventsinthiseventb->Fill();
			//mrdtree->Fill();
		}
		
	}	// end multiple subevents case
	
	//if(eventnum==735){ assert(false); }
	//if(nummrdtracksthisevent) std::this_thread::sleep_for (std::chrono::seconds(5));
	
	// WRITE+CLOSE OUTPUT FILES
	// ========================
	if(verbose){
		cout<<"writing output files, end of finding MRD SubEvents and Tracks in this event"<<endl;
	}
	if(writefile){
		mrdtrackfile->cd();
		mrdtree->SetEntries(nummrdtracksthiseventb->GetEntries());
		mrdtree->Write("",TObject::kOverwrite);
	}
	if(cMRDSubEvent::imgcanvas) cMRDSubEvent::imgcanvas->Update();
	gROOT->cd();
	
	// if not saving to a ROOT file, save to the store for downstream tools.
	m_data->Stores["MRDSubEvents"]->Set("NumMrdSubEvents",nummrdsubeventsthisevent);
	m_data->Stores["MRDSubEvents"]->Set("NumMrdTracks",nummrdtracksthisevent);
	for(int tracki=0; tracki<nummrdsubeventsthisevent; tracki++){
		cMRDSubEvent* asubev = (cMRDSubEvent*)SubEventArray->At(tracki);
		
		// let's not save the SubEvent information. It's not much use.
//		m_data->Stores["MRDSubEvents"]->Set("SubEventID",asubev->GetSubEventID());
//		m_data->Stores["MRDSubEvents"]->Set("NumDigits",asubev->GetNumDigits());
//		m_data->Stores["MRDSubEvents"]->Set("NumLayersHit",asubev->GetNumLayersHit());
//		m_data->Stores["MRDSubEvents"]->Set("NumPmtsHit",asubev->GetNumPMTsHit());
//		m_data->Stores["MRDSubEvents"]->Set("LayersHit",asubev->GetLayersHit());
//		m_data->Stores["MRDSubEvents"]->Set("LayerEdeps",asubev->GetEdeps());
//		m_data->Stores["MRDSubEvents"]->Set("PMTsHit",asubev->GetPMTsHit());
//		//m_data->Stores["MRDSubEvents"]->Set("TrueTracks",asubev->GetTrueTracks()); // WCSimRootTracks
//		m_data->Stores["MRDSubEvents"]->Set("DigitIds",asubev->GetDigitIds());       // WCSimRootCkvDigi indices
//		m_data->Stores["MRDSubEvents"]->Set("DigitQs",asubev->GetDigitQs());
//		m_data->Stores["MRDSubEvents"]->Set("DigitTs",asubev->GetDigitTs());
//		m_data->Stores["MRDSubEvents"]->Set("NumPhotonsInDigits",asubev->GetDigiNumPhots());
//		// since each digit may have mutiple photons the following are concatenated.
//		// scan through and keep a running total of NumPhots to identify the subarray start/ends
//		m_data->Stores["MRDSubEvents"]->Set("PhotTsInDigits",asubev->GetDigiPhotTs());
//		m_data->Stores["MRDSubEvents"]->Set("PhotParentsInDigits",asubev->GetDigiPhotParents());
		
		// can't nest multi-entry BoostStores
		//m_data->Stores["MRDSubEvents"]->Set("RecoTracks",asubev->GetTracks());       // cMRDTracks
		std::vector<cMRDTrack>* thetracks = asubev->GetTracks();
		for(int j=0; j<thetracks->size(); j++){
			cMRDTrack* atrack = &thetracks->at(j);
			
			m_data->Stores["MRDTracks"]->Set("MRDTrackID",atrack->GetTrackID());
			m_data->Stores["MRDTracks"]->Set("MrdSubEventID",atrack->GetMrdSubEventID());
			// particle properties
			m_data->Stores["MRDTracks"]->Set("KEStart",atrack->GetKEStart());
			m_data->Stores["MRDTracks"]->Set("KEEnd",atrack->GetKEEnd());
			m_data->Stores["MRDTracks"]->Set("ParticlePID",atrack->GetParticlePID());
			m_data->Stores["MRDTracks"]->Set("TrackAngle",atrack->GetTrackAngle());
			m_data->Stores["MRDTracks"]->Set("InterceptsTank",atrack->GetInterceptsTank());
			m_data->Stores["MRDTracks"]->Set("StartTime",atrack->GetStartTime());
			Position startpos(atrack->GetStartVertex().X(), atrack->GetStartVertex().Y(), atrack->GetStartVertex().Z());
			Position endpos(atrack->GetStopVertex().X(),atrack->GetStopVertex().Y(),atrack->GetStopVertex().Z());
			m_data->Stores["MRDTracks"]->Set("StartVertex",startpos);
			m_data->Stores["MRDTracks"]->Set("StopVertex",endpos);
			// track properties
			m_data->Stores["MRDTracks"]->Set("TrackAngleError",atrack->GetTrackAngleError());
			//m_data->Stores["MRDTracks"]->Set("TankExitPoint",atrack->GetTankExitPoint());
			m_data->Stores["MRDTracks"]->Set("NumPMTsHit",atrack->GetNumPMTsHit());
			m_data->Stores["MRDTracks"]->Set("NumLayersHit",atrack->GetNumLayersHit());
			m_data->Stores["MRDTracks"]->Set("LayersHit",atrack->GetLayersHit());
			m_data->Stores["MRDTracks"]->Set("TrackLength",atrack->GetTrackLength());
			m_data->Stores["MRDTracks"]->Set("IsMrdPenetrating",atrack->GetIsPenetrating());
			m_data->Stores["MRDTracks"]->Set("IsMrdStopped",atrack->GetIsStopped());
			m_data->Stores["MRDTracks"]->Set("IsMrdSideExit",atrack->GetIsSideExit());
			m_data->Stores["MRDTracks"]->Set("PenetrationDepth",atrack->GetPenetrationDepth());
			
//			m_data->Stores["MRDTracks"]->Set("LayerEdeps",atrack->GetEdeps());
//			m_data->Stores["MRDTracks"]->Set("NumDigits",atrack->GetNumDigits());
//			m_data->Stores["MRDTracks"]->Set("PMTsHit",atrack->GetPMTsHit());
//			m_data->Stores["MRDTracks"]->Set("MrdEntryPoint",atrack->GetMrdEntryPoint());
//			m_data->Stores["MRDTracks"]->Set("MrdEntryBoundsX",atrack->GetMrdEntryBoundsX());
//			m_data->Stores["MRDTracks"]->Set("MrdEntryBoundsY",atrack->GetMrdEntryBoundsY());
//			//m_data->Stores["MRDTracks"]->Set("TrueTrackID",atrack->GetTrueTrackID());
//			//m_data->Stores["MRDTracks"]->Set("TrueTrack",atrack->GetTrueTrack());
//			m_data->Stores["MRDTracks"]->Set("HtrackOrigin",atrack->GetHtrackOrigin());
//			m_data->Stores["MRDTracks"]->Set("HtrackOriginError",atrack->GetHtrackOriginError());
//			m_data->Stores["MRDTracks"]->Set("HtrackGradient",atrack->GetHtrackGradient());
//			m_data->Stores["MRDTracks"]->Set("HtrackGradientError",atrack->GetHtrackGradientError());
//			m_data->Stores["MRDTracks"]->Set("HtrackFitChi2",atrack->GetHtrackFitChi2());
//			m_data->Stores["MRDTracks"]->Set("HtrackFitCov",atrack->GetHtrackFitCov());
//			m_data->Stores["MRDTracks"]->Set("VtrackOrigin",atrack->GetVtrackOrigin());
//			m_data->Stores["MRDTracks"]->Set("VtrackOriginError",atrack->GetVtrackOriginError());
//			m_data->Stores["MRDTracks"]->Set("VtrackGradient",atrack->GetVtrackGradient());
//			m_data->Stores["MRDTracks"]->Set("VtrackGradientError",atrack->GetVtrackGradientError());
//			m_data->Stores["MRDTracks"]->Set("VtrackFitChi2",atrack->GetVtrackFitChi2());
//			m_data->Stores["MRDTracks"]->Set("VtrackFitCov",atrack->GetVtrackFitCov());
//			m_data->Stores["MRDTracks"]->Set("DigitIds",atrack->GetDigitIds());
//			m_data->Stores["MRDTracks"]->Set("DigitQs",atrack->GetDigitQs());
//			m_data->Stores["MRDTracks"]->Set("DigitTs",atrack->GetDigitTs());
//			m_data->Stores["MRDTracks"]->Set("DigiNumPhots",atrack->GetDigiNumPhots());
//			m_data->Stores["MRDTracks"]->Set("DigiPhotTs",atrack->GetDigiPhotTs());
//			m_data->Stores["MRDTracks"]->Set("DigiPhotParents",atrack->GetDigiPhotParents());
		}
	}
	m_data->Stores["MRDSubEvents"]->Save();
	
	return true;
}

void FindMrdTracks::StartNewFile(){
	TString filenameout = TString::Format("%s/mrdtrackfile.%d.%d.root",outputdir.c_str(),runnum,subrunnum);
	if(verbose) cout<<"creating mrd output file "<<filenameout.Data()<<endl;
	if(mrdtrackfile) mrdtrackfile->Close();
	mrdtrackfile = new TFile(filenameout.Data(),"RECREATE","MRD Tracks file");
	mrdtrackfile->cd();
	mrdtree = new TTree("mrdtree","Tree for reconstruction data");
	mrdeventnumb = mrdtree->Branch("EventID",&eventnum);
	mrdtriggernumb = mrdtree->Branch("TriggerID",&triggernum);
	nummrdsubeventsthiseventb = mrdtree->Branch("nummrdsubeventsthisevent",&nummrdsubeventsthisevent);
	subeventsinthiseventb = mrdtree->Branch("subeventsinthisevent",&SubEventArray, nummrdsubeventsthisevent);
	nummrdtracksthiseventb = mrdtree->Branch("nummrdtracksthisevent",&nummrdtracksthisevent);
	gROOT->cd();
}


bool FindMrdTracks::Finalise(){
	//SubEventArray->Clear("C");
	if(mrdtrackfile){
		mrdtrackfile->Close();
		delete mrdtrackfile;
		mrdtrackfile=nullptr;
	}
	//if(SubEventArray){ SubEventArray->Delete(); delete SubEventArray; SubEventArray=0;}
	return true;
}
