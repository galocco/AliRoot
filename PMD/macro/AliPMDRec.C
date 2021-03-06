void AliPMDRec()
{
  // This macro for the full reconstruction chain. Only PMD is ON.
  //
  AliCDBManager * man = AliCDBManager::Instance();
  man->SetDefaultStorage("local://$ALICE_ROOT/OCDB");
  man->SetRun(0);


  Int_t firstEvent = 0;
  Int_t lastEvent = 1;

  AliReconstruction rec;
  rec.SetRunReconstruction("PMD");
  rec.SetRunVertexFinder(kFALSE);
  rec.SetFillESD("PMD");
  rec.SetFillTriggerESD(kFALSE);

  //  rec.Run("./");
  //  rec.Run("raw.date");
  rec.Run("raw_6b_61.root",firstEvent,lastEvent);

}

