#ifndef ALIRECONSTRUCTOR_H
#define ALIRECONSTRUCTOR_H
/* Copyright(c) 1998-1999, ALICE Experiment at CERN, All rights reserved. *
 * See cxx source for full Copyright notice                               */

/* $Id$ */

#include <TObject.h>

class AliRunLoader;
class AliVertexer;
class AliTracker;
class AliESD;


class AliReconstructor: public TObject {
public:
  virtual void         Reconstruct(AliRunLoader* runLoader) const = 0;
  virtual AliVertexer* CreateVertexer(AliRunLoader* /*runLoader*/) const 
    {return NULL;}
  virtual AliTracker*  CreateTracker(AliRunLoader* /*runLoader*/) const 
    {return NULL;}
  virtual void         FillESD(AliRunLoader* runLoader, AliESD* esd) const = 0;

  ClassDef(AliReconstructor, 0)   // base class for reconstruction algorithms
};

#endif
