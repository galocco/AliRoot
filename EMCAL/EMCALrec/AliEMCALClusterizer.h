#ifndef ALIEMCALCLUSTERIZER_H
#define ALIEMCALCLUSTERIZER_H
/* Copyright(c) 1998-1999, ALICE Experiment at CERN, All rights reserved. *
 * See cxx source for full Copyright notice                               */
                            
//_________________________________________________________________________
/// \class AliEMCALClusterizer
/// \ingroup EMCALrec
/// \brief Base class for the clusterization algorithm (pure abstract)
/// 
///  Clusterization mother class. Contains common methods/data members of different 
///  clusterizers. 
///
/// \author: Yves Schutz (SUBATECH) 
/// \author Dmitri Peressounko (SUBATECH & Kurchatov Institute)
//_________________________________________________________________________

// --- ROOT system ---
#include <TObject.h>
#include <TClonesArray.h>
class TTree;

// --- AliRoot header files ---
#include "AliLog.h"
class AliEMCALGeometry;
class AliEMCALCalibData;
class AliEMCALCalibTime;
class AliCaloCalibPedestal;
class AliEMCALRecParam;
#include "AliEMCALUnfolding.h"

class AliEMCALClusterizer : public TObject 
{

public:

  AliEMCALClusterizer();
  AliEMCALClusterizer(AliEMCALGeometry *geometry);
  AliEMCALClusterizer(AliEMCALGeometry *geometry, AliEMCALCalibData *calib, 
                      AliEMCALCalibTime * calibt, AliCaloCalibPedestal *pedestal);
  virtual ~AliEMCALClusterizer();

  // Main methods

  virtual void    DeleteDigits();
  virtual void    DeleteRecPoints();

  virtual void    Digits2Clusters(Option_t *option) = 0;

  virtual void    Calibrate(Float_t & amp, Float_t & time, const Int_t cellId);
  virtual void    Init();
  virtual void    InitParameters();
  virtual void    InitParameters(const AliEMCALRecParam* recParam);

  virtual void    Print         (Option_t *option)   const ;
  virtual void    PrintRecPoints(Option_t *option);
  virtual void    PrintRecoInfo();

  virtual const char *Version()                      const { Warning("Version", "Not Defined"); 
                                                             return 0 ;                        } 

  // Getters-Setters

  virtual void    SetInput (TTree *digitsTree  );
  virtual void    SetOutput(TTree *clustersTree);

  virtual void    GetCalibrationParameters(void);
  virtual void    GetCaloCalibPedestal(void);
  virtual void    SetCalibrationParameters(AliEMCALCalibData *calib)     { fCalibData = calib;   }
  virtual void    SetTimeCalibrationParameters(AliEMCALCalibTime *calib) { fCalibTime = calib;   }
  virtual void    SetCaloCalibPedestal(AliCaloCalibPedestal  *caped)     { fCaloPed   = caped;   }
  
  virtual Float_t GetTimeMin()                        const { return fTimeMin;                 }
  virtual Float_t GetTimeMax()                        const { return fTimeMax;                 }
  virtual Float_t GetTimeCut()                        const { return fTimeCut;                 }
  virtual Float_t IsTimeCalibrationOn()               const { return fTimeCalibration;         }
  virtual Float_t GetECAClusteringThreshold()         const { return fECAClusteringThreshold;  }  
  virtual Float_t GetECALocalMaxCut()                 const { return fECALocMaxCut;            } 
  virtual Float_t GetECALogWeight()                   const { return fECAW0;                   }
  virtual Float_t GetMinECut()                        const { return fMinECut;                 } 
  virtual Bool_t  GetRejectBelowThreshold()           const { return fRejectBelowThreshold;    }

  virtual void    SetTimeMin(Float_t t)                     { fTimeMin = t;                    }
  virtual void    SetTimeMax(Float_t t)                     { fTimeMax = t;                    }
  virtual void    SetTimeCut(Float_t t)                     { fTimeCut = t;                    }
  virtual void    SetTimeCalibration(Bool_t t)              { fTimeCalibration = t;            }
  
  virtual void    SetECAClusteringThreshold(Float_t th)     { fECAClusteringThreshold = th;    }
  virtual void    SetMinECut(Float_t mine)                  { fMinECut      = mine;            }
  virtual void    SetECALocalMaxCut(Float_t cut)            { fECALocMaxCut = cut;             }
  virtual void    SetECALogWeight(Float_t w)                { fECAW0        = w;               }
  virtual void    SetRejectBelowThreshold(Bool_t reject)    { fRejectBelowThreshold = reject;  }
  
  // Unfolding

  virtual void    SetUnfolding(Bool_t toUnfold = kTRUE )    { fToUnfold = toUnfold;            }  
  virtual void    SetSSPars   (Int_t ipar, Double_t par)    { fSSPars[ipar] = par;             }
  virtual void    SetPar5     (Int_t ipar, Double_t par)    { fPar5  [ipar] = par;             }
  virtual void    SetPar6     (Int_t ipar, Double_t par)    { fPar6  [ipar] = par;             }
  
  virtual void    InitClusterUnfolding()                    {
    fClusterUnfolding=new AliEMCALUnfolding(fGeom,fECALocMaxCut,fSSPars,fPar5,fPar6); 
    fClusterUnfolding->SetThreshold(fMinECut);
    fClusterUnfolding->SetRejectBelowThreshold(fRejectBelowThreshold);                         }

  // NxN (only used in NxN clusterizer)
  
  virtual void    SetNRowDiff(Int_t )                       { ; }
  virtual void    SetNColDiff(Int_t )                       { ; }
  virtual void    SetEnergyGrad(Bool_t )                    { ; }

  virtual Int_t   GetNRowDiff()                       const { return -1 ; }
  virtual Int_t   GetNColDiff()                       const { return -1 ; } 
  virtual Bool_t  GetEnergyGrad()                     const { return -1 ; }

  // add for clusterizing task

  virtual void                SetDigitsArr(TClonesArray *arr) { fDigitsArr = arr  ;            }
  virtual TClonesArray       *GetDigits()                     
  { if (!fDigitsArr) fDigitsArr = new TClonesArray("AliEMCALDigit",12000);
    return fDigitsArr ; }
  
  virtual const TObjArray    *GetRecPoints()            const { return fRecPoints ;            }
  void                        SetInputCalibrated(Bool_t val);
  void                        SetJustClusters   (Bool_t val);
  
protected:

  virtual void MakeClusters() = 0;
  
  Bool_t   fIsInputCalibrated;          ///<  to enable reclusterization from ESD cells
  Bool_t   fJustClusters;               ///<  false for standard reco  
  TClonesArray         *fDigitsArr;     ///<  array with EMCAL digits
  TTree                *fTreeR;         ///<  tree with output clusters
  TObjArray            *fRecPoints;     ///<  array with EMCAL clusters
  
  AliEMCALGeometry     *fGeom;          //!<!  pointer to geometry for utilities
  AliEMCALCalibData    *fCalibData;     //!<!  energy calibration database if aval
  AliEMCALCalibTime    *fCalibTime;     //!<!  time calibration database if aval
  AliCaloCalibPedestal *fCaloPed;       //!<!  tower status map if aval
  
  Float_t  fADCchannelECA;              ///<  width of one ADC channel for EC section (GeV)
  Float_t  fADCpedestalECA;             ///<  pedestal of ADC for EC section (GeV) 
  Float_t  fTimeECA;                    ///<  calibration parameter for channels time

  Float_t  fTimeMin;                    ///<  minimum time of physical signal in a cell/digit
  Float_t  fTimeMax;                    ///<  maximum time of physical signal in a cell/digit
  Float_t  fTimeCut;                    ///<  maximum time difference between the digits inside EMC cluster
  Bool_t   fTimeCalibration;            ///<  recover time shifts from OCDB?

  Bool_t   fDefaultInit;                //!<! says if the task was created by defaut ctor (only parameters are initialized)
  Bool_t   fToUnfold;                   ///<  says if unfolding should be performed 
  Int_t    fNumberOfECAClusters;        ///<  number of clusters found in EC section
  
  Float_t  fECAClusteringThreshold;     ///<  minimum energy to seed a EC digit in a cluster
  Float_t  fECALocMaxCut;               ///<  minimum energy difference to distinguish local maxima in a cluster
  Float_t  fECAW0;                      ///<  logarithmic weight for the cluster center of gravity calculation
  Float_t  fMinECut;                    ///<  minimum energy for a digit to be a member of a cluster
  Bool_t   fRejectBelowThreshold;       ///<  split (false-default) or reject (true) cell energy below threshold after UF 
  
  AliEMCALUnfolding *fClusterUnfolding; //!<! pointer to unfolding object
  Double_t fSSPars[8];                  ///<  shower shape parameters 
  Double_t fPar5[3];                    ///<  shower shape parameter 5
  Double_t fPar6[3];                    ///<  shower shape parameter 6

 private:
  
  AliEMCALClusterizer(              const AliEMCALClusterizer &);
  AliEMCALClusterizer & operator = (const AliEMCALClusterizer &);
  
  /// \cond CLASSIMP
  ClassDef(AliEMCALClusterizer,10) ; 
  /// \endcond

};
#endif // AliEMCALCLUSTERIZER_H
