// Copyright (C) 2011 Technische Universitaet Muenchen
// This file is part of the preCICE project. For conditions of distribution and
// use, please see the license notice at http://www5.in.tum.de/wiki/index.php/PreCICE_License
#include "BaseQNPostProcessing.hpp"
#include "cplscheme/CouplingData.hpp"
#include "utils/Globals.hpp"
#include "tarch/la/MatrixVectorOperations.h"
#include "mesh/Mesh.hpp"
#include "mesh/Vertex.hpp"
#include "utils/Dimensions.hpp"
#include "tarch/la/Scalar.h"
#include "io/TXTWriter.hpp"
#include "io/TXTReader.hpp"
//#include "utils/NumericalCompare.hpp"

#include <time.h>

namespace precice {
namespace cplscheme {
namespace impl {

tarch::logging::Log BaseQNPostProcessing::
      _log("precice::cplscheme::impl::BaseQNPostProcessing");

      
/* ----------------------------------------------------------------------------
 *     Constructor
 * ----------------------------------------------------------------------------
 */ 
BaseQNPostProcessing:: BaseQNPostProcessing
(
  double initialRelaxation,
  int    maxIterationsUsed,
  int    timestepsReused,
  double singularityLimit,
  std::vector<int> dataIDs,
  std::map<int,double> scalings)
:
  PostProcessing(),
  _initialRelaxation(initialRelaxation),
  _maxIterationsUsed(maxIterationsUsed),
  _timestepsReused(timestepsReused),
  _singularityLimit(singularityLimit),
  _dataIDs(dataIDs),
  _secondaryDataIDs(),
  _scalings(scalings),
  _firstIteration(true),
  _firstTimeStep(true),
  _oldXTilde(),
  //_secondaryOldXTildes(),
  _residuals(),
  _secondaryResiduals(),
  _scaledValues(),
  _scaledOldValues(),
  _oldResiduals(),
  _matrixV(),
  _matrixW(),
  _matrixVBackup(),
  _matrixWBackup(),
  _matrixColsBackup(),
  //_secondaryMatricesW(),
  _timingStream(),
  _matrixCols()
{
   preciceCheck((_initialRelaxation > 0.0) && (_initialRelaxation <= 1.0),
                "BaseQNPostProcessing()",
                "Initial relaxation factor for QN post-processing has to "
                << "be larger than zero and smaller or equal than one!");
   preciceCheck(_maxIterationsUsed > 0, "BaseQNPostProcessing()",
                "Maximal iterations used for QN post-processing has to "
                << "be larger than zero!");
   preciceCheck(_timestepsReused >= 0, "BaseQNPostProcessing()",
                "Number of old timesteps to be reused for QN "
                 << "post-processing has to be >= 0!");
   preciceCheck(tarch::la::greater(_singularityLimit, 0.0),
                "BaseQNPostProcessing()", "Singularity limit for QN "
                << "post-processing has to be larger than numerical zero ("
                << tarch::la::NUMERICAL_ZERO_DIFFERENCE << ")!");
   
   
  _timingStream.open ("timing.txt");
  _timingStream.setf ( std::ios::showpoint );
  _timingStream.setf ( std::ios::fixed );
  _timingStream << std::setprecision(16);
  _timingStream << "        Time Measurements \n -------------------------------------------------------- \n\n";
}


/* ----------------------------------------------------------------------------
 *     initialize
 * ----------------------------------------------------------------------------
 */ 
void BaseQNPostProcessing:: initialize
(
  DataMap& cplData )
{
  preciceTrace1("initialize()", cplData.size());
  size_t entries=0;

  for(size_t i=0;i<_dataIDs.size();i++){
    preciceCheck(utils::contained(_dataIDs[i], cplData), "initialize()",
                 "Data with ID " << _dataIDs[i] << " is not contained in data "
                 "given at initialization!");
    entries += cplData[_dataIDs[i]]->values->size();
  }



  assertion(entries > 0);
  double init = 0.0;
  assertion(_oldXTilde.size() == 0);
  assertion(_oldResiduals.size() == 0);
  _oldXTilde.append(DataValues(entries, init));
  _oldResiduals.append(DataValues(entries, init));
  _residuals.append(DataValues(entries, init));
  _scaledValues.append(DataValues(entries, init));
  _scaledOldValues.append(DataValues(entries, init));
  _matrixCols.push_front(0);
  _firstIteration = true;
  _firstTimeStep = true;

  // Fetch secondary data IDs, to be relaxed with same coefficients from IQN-ILS
  foreach (DataMap::value_type& pair, cplData){
    if (not utils::contained(pair.first, _dataIDs)){
      _secondaryDataIDs.push_back(pair.first);
      int secondaryEntries = pair.second->values->size();
//      _secondaryOldXTildes[pair.first].append(DataValues(secondaryEntries, init));
      _secondaryResiduals[pair.first].append(DataValues(secondaryEntries, init));
    }
  }

  // Append old value columns, if not done outside of post-processing already
  foreach (DataMap::value_type& pair, cplData){
    int cols = pair.second->oldValues.cols();
    if (cols < 1){ // Add only, if not already done
      assertion1(pair.second->values->size() > 0, pair.first);
      pair.second->oldValues.append(
        CouplingData::DataMatrix(pair.second->values->size(), 1, 0.0));
    }
  }
}


/* ----------------------------------------------------------------------------
 *     scaling
 * ----------------------------------------------------------------------------
 */
void BaseQNPostProcessing:: scaling
(
  DataMap& cplData)
{
  preciceTrace("scaling()");
  
  int offset = 0;
  double l2norm = 0.;
  double oldl2norm = 0.;
  foreach (int id, _dataIDs){
    l2norm = 0.; // debug
    double factor = _scalings[id];
    preciceDebug("Scaling Factor " << factor << " for id: " << id);
    int size = cplData[id]->values->size();
    DataValues& values = *cplData[id]->values;
    DataValues& oldValues = cplData[id]->oldValues.column(0);
    for (int i=0; i < size; i++){
      _scaledValues[i+offset] = values[i]/factor;
      _scaledOldValues[i+offset] = oldValues[i]/factor;
      
      // debug:
      l2norm += _scaledValues[i+offset]*_scaledValues[i+offset];
    }
    offset += size;
    
    // debug:
    if (id == 0) oldl2norm = sqrt(l2norm);
    preciceDebug(" + l2-Norm = " << sqrt(l2norm) << " of id: " << id);
  } 
  // debug:
  preciceDebug(" + l2-Norm ratio = "<< oldl2norm/sqrt(l2norm));
}

/* ----------------------------------------------------------------------------
 *     undoScaling
 * ----------------------------------------------------------------------------
 */
void BaseQNPostProcessing:: undoScaling
(
  DataMap& cplData)
{
  preciceTrace("undoScaling()");
  
  int offset = 0;
  foreach(int id, _dataIDs){
    double factor = _scalings[id];
    int size = cplData[id]->values->size();
    preciceDebug("Copying values back, size: " << size);
    utils::DynVector& valuesPart = *(cplData[id]->values);
    utils::DynVector& oldValuesPart = cplData[id]->oldValues.column(0);
    for(int i=0; i < size; i++){
      valuesPart[i] = _scaledValues[i+offset]*factor;
      oldValuesPart[i] = _scaledOldValues[i+offset]*factor;
    }
    offset += size;
  }
}


/* ----------------------------------------------------------------------------
 *     updateDiffernceMatrices
 * ----------------------------------------------------------------------------
 */
void BaseQNPostProcessing:: updateDifferenceMatrices
(
  DataMap& cplData)
{
  preciceTrace("updateDiffernceMatrices()");
  using namespace tarch::la;

  
  // Compute current residual: vertex-data - oldData
  //DataValues residuals(scaledValues);
  _residuals = _scaledValues;
  _residuals -= _scaledOldValues;

  //if (_firstIteration && (_matrixCols.size() < 2)){
  /*
   * ATTETION: changed the condition from _firstIteration && _firstTimeStep
   * to the following: 
   * underrelaxation has to be done, if the scheme has converged without even
   * entering post processing. In this case the V, W matrices would still be empty.
   * This case happended in the open foam example beamInCrossFlow.
   */ 
  if(_firstIteration && (_firstTimeStep ||  (_matrixCols.size() < 2))){
//     preciceDebug("   Performing underrelaxation");
//     _oldXTilde = _scaledValues; // Store x tilde
//     _oldResiduals = _residuals; // Store current residual
//     // Perform underrelaxation with residual: x_new = x_old + omega * res
//     _residuals *= _initialRelaxation;
//     _residuals += _scaledOldValues;
//     _scaledValues = _residuals;

  }
  else {
    //preciceDebug("   Performing QN step");

    if (not _firstIteration){ // Update matrices V, W with newest information
      assertion2(_matrixV.cols() == _matrixW.cols(), _matrixV.cols(), _matrixW.cols());
      assertion2(_matrixV.cols() <= _maxIterationsUsed,
                 _matrixV.cols(), _maxIterationsUsed);
      bool columnLimitReached = _matrixV.cols() == _maxIterationsUsed;
      bool overdetermined = _matrixV.cols() <= _matrixV.rows();
      if (not columnLimitReached && overdetermined){
        _matrixV.appendFront(_residuals); // Will be modified to delta_r
        _matrixW.appendFront(_residuals); // Will be overwritten by delta_x_tilde
        
        _matrixCols.front()++;
      }
      else {
        _matrixV.shiftSetFirst(_residuals); // Will be modified to delta_r
        _matrixW.shiftSetFirst(_residuals); // Will be overwritten by delta_x_tilde
	
	_matrixCols.front()++;
        _matrixCols.back()--;
        if (_matrixCols.back() == 0){
          _matrixCols.pop_back();
        }
      }
      // Compute delta_residual = residual - residual_old
      _matrixV.column(0) -= _oldResiduals;

      // Compute delta_x_tilde = x_tilde - x_tilde_old
      _matrixW.column(0) = _scaledValues;
      _matrixW.column(0) -= _oldXTilde;
      
    }

    _oldResiduals = _residuals;   // Store residuals
    _oldXTilde = _scaledValues;   // Store x_tilde
  }
  
}



/* ----------------------------------------------------------------------------
 *     performPostProcessing
 * ----------------------------------------------------------------------------
 */
void BaseQNPostProcessing:: performPostProcessing
(
  DataMap& cplData)
{
  preciceTrace2("performPostProcessing()", _dataIDs.size(), cplData.size());
  using namespace tarch::la;
  assertion2(_dataIDs.size() == _scalings.size(), _dataIDs.size(), _scalings.size());
  assertion2(_oldResiduals.size() == _oldXTilde.size(),
             _oldResiduals.size(), _oldXTilde.size());
  assertion2(_scaledValues.size() == _oldXTilde.size(),
             _scaledValues.size(), _oldXTilde.size());
  assertion2(_scaledOldValues.size() == _oldXTilde.size(),
             _scaledOldValues.size(), _oldXTilde.size());
  assertion2(_residuals.size() == _oldXTilde.size(),
             _residuals.size(), _oldXTilde.size());


 double time_pPP = clock();
 double time_QNStep = 0.;
 double time_scaleUpdateVW = time_pPP;
 double time_norelaxAux = 0.;
 double time_scaleUpdateVW_tmp = 0.;
 
  // scale data values (and secondary data values)
  scaling(cplData);
  
  // perform post processing for secondary data
  //performPPSecondaryData(cplData);
  
  /** update the difference matrices V,W  includes: 
    * scaling of values
    * computation of residuals
    * appending the difference matrices 
    */
  updateDifferenceMatrices(cplData);
  
  time_scaleUpdateVW = clock() - time_scaleUpdateVW;
  time_scaleUpdateVW /= CLOCKS_PER_SEC; 
  
//   int offset = 0;
//   foreach (int id, _dataIDs){
//     double factor = _scalings[id];
//     preciceDebug("Scaling Factor " << factor << " for id: " << id);
//     int size = cplData[id]->values->size();
//     DataValues& values = *cplData[id]->values;
//     DataValues& oldValues = cplData[id]->oldValues.column(0);
//     for (int i=0; i < size; i++){
//       _scaledValues[i+offset] = values[i]/factor;
//       _scaledOldValues[i+offset] = oldValues[i]/factor;
//     }
//     offset += size;
//   }
// 
//   // Compute current residual: vertex-data - oldData
//   //DataValues residuals(scaledValues);
//   _residuals = _scaledValues;
//   _residuals -= _scaledOldValues;
// 
//   //if (_firstIteration && (_matrixCols.size() < 2)){
//   /*
//    * ATTETION: changed the condition from _firstIteration && _firstTimeStep
//    * to the following: 
//    * underrelaxation has to be done, if the scheme has converged without even
//    * entering post processing. In this case the V, W matrices would still be empty.
//    * This case happended in the open foam example beamInCrossFlow.
//    */ 
   if(_firstIteration && (_firstTimeStep ||  (_matrixCols.size() < 2))){
     preciceDebug("   Performing underrelaxation");
     _oldXTilde = _scaledValues; // Store x tilde
     _oldResiduals = _residuals; // Store current residual
     // Perform underrelaxation with residual: x_new = x_old + omega * res
     _residuals *= _initialRelaxation;
     _residuals += _scaledOldValues;
     _scaledValues = _residuals;
     
     // compute underrelaxation for the secondary data
     computeUnderrelaxationSecondaryData(cplData);
 
   }
   else {
     preciceDebug("   Performing QN step");
// 
//     if (not _firstIteration){ // Update matrices V, W with newest information
//       assertion2(_matrixV.cols() == _matrixW.cols(), _matrixV.cols(), _matrixW.cols());
//       assertion2(_matrixV.cols() <= _maxIterationsUsed,
//                  _matrixV.cols(), _maxIterationsUsed);
//       bool columnLimitReached = _matrixV.cols() == _maxIterationsUsed;
//       bool overdetermined = _matrixV.cols() <= _matrixV.rows();
//       if (not columnLimitReached && overdetermined){
//         _matrixV.appendFront(_residuals); // Will be modified to delta_r
//         _matrixW.appendFront(_residuals); // Will be overwritten by delta_x_tilde
//         
//         _matrixCols.front()++;
//       }
//       else {
//         _matrixV.shiftSetFirst(_residuals); // Will be modified to delta_r
//         _matrixW.shiftSetFirst(_residuals); // Will be overwritten by delta_x_tilde
// 	
// 	_matrixCols.front()++;
//         _matrixCols.back()--;
//         if (_matrixCols.back() == 0){
//           _matrixCols.pop_back();
//         }
//       }
//       // Compute delta_residual = residual - residual_old
//       _matrixV.column(0) -= _oldResiduals;
// 
//       // Compute delta_x_tilde = x_tilde - x_tilde_old
//       _matrixW.column(0) = _scaledValues;
//       _matrixW.column(0) -= _oldXTilde;
//       
//     }
//     
// 
//     _oldResiduals = _residuals;   // Store residuals
//     _oldXTilde = _scaledValues;   // Store x_tilde
  
    time_norelaxAux = clock();
     
     //std::cout<<"v.cols = "<<_matrixV.cols()<<" w.cols = "<<_matrixW.cols()<<std::endl;
    if((_matrixV.cols() < 1 || _matrixW.cols()) < 1 && _timestepsReused == 0)
    {
     _matrixV = _matrixVBackup;
     _matrixW = _matrixWBackup;
     _matrixCols = _matrixColsBackup;
    }

    DataValues xUpdate(_residuals.size(), 0.0);
    
    time_QNStep = clock();
    
    /**
     * compute quasi-Newton update 
     */
    computeQNUpdate(cplData, xUpdate);
    
    time_QNStep = clock() - time_QNStep;
    time_QNStep /= CLOCKS_PER_SEC;
  
    /** 
     * apply quasiNewton update
     */
    _scaledValues = _scaledOldValues;  // = x^k
    _scaledValues += xUpdate;        // = x^k + delta_x
    _scaledValues += _residuals; // = x^k + delta_x + r^k
    
    
    // pending deletion: delete old V, W matrices if timestepsReused = 0
    // those were only deeded for the first iteration (instead of underrelax.)
    //std::cout<<"first iteration = "<<_firstIteration<<"  first time step = "<<_firstTimeStep<<std::endl;
    if(_firstIteration && _timestepsReused == 0)
    {
      if(_matrixV.cols() > 0 && _matrixW.cols() > 0)
      {
	_matrixColsBackup = _matrixCols;
        _matrixVBackup = _matrixV;
        _matrixWBackup = _matrixW;
      }
      if(not _firstTimeStep)
      {
        _matrixV.clear();
        _matrixW.clear();
        _matrixCols.clear();
      }
          // TOD0: The following is still misssing for reusedTimeSTeps=0 
    // -----------------------------------------------
// // //     foreach (int id, _secondaryDataIDs){
// // //       _secondaryMatricesW[id].clear();
// // //     }
    // -----------------------------------------------
    }
    
    time_norelaxAux = clock() - time_norelaxAux;
    time_norelaxAux /= CLOCKS_PER_SEC;
    time_norelaxAux -= time_QNStep;

  }

  time_scaleUpdateVW_tmp = clock();
  
  // Undo scaling of data values and overwrite originals
  undoScaling(cplData);
  
  time_scaleUpdateVW_tmp = clock() - time_scaleUpdateVW_tmp;
  time_scaleUpdateVW_tmp /= CLOCKS_PER_SEC;
  time_scaleUpdateVW += time_scaleUpdateVW_tmp;
  
  time_pPP = clock() - time_pPP;
  time_pPP /= CLOCKS_PER_SEC;
  
  _timingStream << "\n";
  _timingStream << " time for scaling and update ov V, W matrices: "<<time_scaleUpdateVW<<"\n";
  _timingStream << " time for norelax auxiliaries: "<<time_norelaxAux<<"\n";
  _timingStream << " time for QN update: "<<time_QNStep<<"\n";
  _timingStream << " time for perform post processing (accumulated): "<<time_pPP<<"\n";
  _timingStream << "----------------------------\n";
  
  _firstIteration = false;
}

void BaseQNPostProcessing:: iterationsConverged
(
   DataMap & cplData)
{
  preciceTrace("iterationsConverged()");
  
  // the most recent differences for the V, W matrices have not been added so far
  // this has to be done in iterations converged, as PP won't be called if 
  // convergence achieved
  if(not _timestepsReused == 0)
  {
    scaling(cplData);
    updateDifferenceMatrices(cplData);
    undoScaling(cplData);
  }
  
# ifdef Debug
  std::ostringstream stream;
  stream << "Matrix column counters: ";
  foreach (int cols, _matrixCols){
    stream << cols << ", ";
  }
  preciceDebug(stream.str());
# endif // Debug

  _firstTimeStep = false;
  if (_matrixCols.front() == 0){ // Did only one iteration
    _matrixCols.pop_front(); 
  }
  
  // doing specialized stuff for the corresponding post processing sceme after 
  // convergence of iteration i.e.:
  // - analogously to the V,W matrices, remove columns from matrices for secondary data
  // - save the old jacobian matrix i
  specializedIterationsConverged(cplData);

  if (_timestepsReused == 0){
    
    /**
     * pending deletion (after first iteration of next time step
     * Using the matrices from the old time step for the first iteration
     * is better than doing underrelaxation as first iteration of every time step
     */
  }
  else if ((int)_matrixCols.size() > _timestepsReused){
    int toRemove = _matrixCols.back();
    assertion1(toRemove > 0, toRemove);
    preciceDebug("Removing " << toRemove << " cols from IQN-ILS matrices with "
                 << _matrixV.cols() << " cols");
    assertion2(_matrixV.cols() == _matrixW.cols(), _matrixV.cols(), _matrixW.cols());
    assertion2(_matrixV.cols() > toRemove, _matrixV.cols(), toRemove);
    for (int i=0; i < toRemove; i++){
      _matrixV.remove(_matrixV.cols() - 1);
      _matrixW.remove(_matrixW.cols() - 1);
    }
    _matrixCols.pop_back();
  }
  _matrixCols.push_front(0);
  _firstIteration = true;
}

void BaseQNPostProcessing:: exportState(io::TXTWriter& writer)
{
//  tarch::la::Vector<1,int> colSize(_matrixCols.size());
//  writer.write(colSize);
//  writer.write(_matrixCols);
//  writer.write(_oldXTilde);
//  writer.write(_oldResiduals);
//  writer.write(_matrixV);
//  writer.write(_matrixW);
}

void BaseQNPostProcessing:: importState(io::TXTReader& reader)
{
//  tarch::la::Vector<1,int> colSize;
//  reader.read(colSize);
//  _matrixCols.resize(colSize[0]);
//  reader.read(_oldXTilde);
//  reader.read(_oldResiduals);
//  for (int i=0; i<colSize[0]; i++ ){
//    // Using _oldXTilde to have a vector with appropriate size.
//    // Values are overwritten afterwards in file read.
//    _matrixV.append(_oldXTilde);
//    _matrixW.append(_oldXTilde);
//  }
//  reader.read(_matrixV);
//  reader.read(_matrixW);
//  if (colSize[0] > 1){
//    _firstIteration = false;
//  }
}

void BaseQNPostProcessing:: removeMatrixColumn
(
  int columnIndex)
{
  preciceTrace2("removeMatrixColumn()", columnIndex, _matrixV.cols());

  // Remove matrix columns
  assertion(_matrixV.cols() > 1);
  _matrixV.remove(columnIndex);
  _matrixW.remove(columnIndex);
  // Reduce column count
  std::deque<int>::iterator iter = _matrixCols.begin();
  int cols = 0;
  while(iter != _matrixCols.end()) {
    cols += *iter;
    if(cols > columnIndex) {
      assertion(*iter > 0);
      *iter -= 1;
      if(*iter == 0) {
        _matrixCols.erase(iter);
      }
      break;
    }
    iter++;
  }
}

}}} // namespace precice, cplscheme, impl