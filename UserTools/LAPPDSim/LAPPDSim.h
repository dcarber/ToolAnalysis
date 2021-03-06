#ifndef LAPPDSim_H
#define LAPPDSim_H

#include <string>
#include <iostream>

#include "Tool.h"
#include "LAPPDresponse.hh"

class LAPPDSim: public Tool {


 public:

  LAPPDSim();
  bool Initialise(std::string configfile,DataModel &data);
  bool Execute();
  bool Finalise();
  Waveform<double> SimpleGenPulse(vector<double> pulsetimes);

 private:

   //ROOT random number generator
   TRandom3* myTR;
   TString SimInput;

   int iter=0;

};


#endif
