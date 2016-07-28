/*
 Copyright (c) 2012 Peter Szepe

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstring>
#include <string>
#include <sstream>

#include <GraphMol/GraphMol.h>
#include <GraphMol/SmilesParse/SmilesParse.h>

#include <tbb/atomic.h>
#include <tbb/partitioner.h>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

#include "main.hpp"
#include "inout.h"
#include "chem/fingerprintStrategy/FingerprintStrategy.h"
#include "chem/simCoefStrategy/SimCoefStrategy.h"
#include "MorphingData.h"
#include "chem/morphingStrategy/MorphingStrategy.h"
#include "chem/morphing/MorphingFtors.hpp"
#include "chem/morphing/Morphing.hpp"

// TODO: merge into one header file ?
#include "chem/morphingStrategy/OpAddAtom.hpp"
#include "chem/morphingStrategy/OpAddBond.hpp"
#include "chem/morphingStrategy/OpBondContraction.hpp"
#include "chem/morphingStrategy/OpBondReroute.hpp"
#include "chem/morphingStrategy/OpInterlayAtom.hpp"
#include "chem/morphingStrategy/OpMutateAtom.hpp"
#include "chem/morphingStrategy/OpRemoveAtom.hpp"
#include "chem/morphingStrategy/OpRemoveBond.hpp"
#include "core/PathFinder.h"

#if MORPHING_REPORTING == 1
#define REPORT_RECOVERY(x) SynchCout((x))
#else
#define REPORT_RECOVERY(x)
#endif

static void InitStrategies(
    std::vector<ChemOperSelector> &chemOperSelectors,
    std::vector<MorphingStrategy *> &strategies)
{
    for (int i = 0; i < chemOperSelectors.size(); ++i) {
        switch (chemOperSelectors[i]) {
        case OP_ADD_ATOM:
            strategies.push_back(new OpAddAtom());
            break;
        case OP_REMOVE_ATOM:
            strategies.push_back(new OpRemoveAtom());
            break;
        case OP_ADD_BOND:
            strategies.push_back(new OpAddBond());
            break;
        case OP_REMOVE_BOND:
            strategies.push_back(new OpRemoveBond());
            break;
        case OP_MUTATE_ATOM:
            strategies.push_back(new OpMutateAtom());
            break;
        case OP_INTERLAY_ATOM:
            strategies.push_back(new OpInterlayAtom());
            break;
        case OP_BOND_REROUTE:
            strategies.push_back(new OpBondReroute());
            break;
        case OP_BOND_CONTRACTION:
            strategies.push_back(new OpBondContraction());
            break;
        default:
            break;
        }
    }
}

void GenerateMorphs(
    MolpherMolecule &candidate,
    unsigned int morphAttempts,
    FingerprintSelector fingerprintSelector,
    SimCoeffSelector simCoeffSelector,
    std::vector<ChemOperSelector> &chemOperSelectors,
    MolpherMolecule &target,
    std::vector<MolpherMolecule> &decoys,
    tbb::task_group_context &tbbCtx ,
    void *callerState,
    void (*deliver)(MolpherMolecule *, void *),
    Scaffold* scaff)
{
    RDKit::RWMol *mol = NULL;
    try {
        mol = RDKit::SmilesToMol(candidate.smile);
        if (mol) {
            RDKit::MolOps::Kekulize(*mol);
        } else {
            throw ValueErrorException("");
        }
    } catch (const ValueErrorException &exc) {
        delete mol;
        return;
    }

    RDKit::RWMol *targetMol = NULL;
    try {
        targetMol = RDKit::SmilesToMol(target.smile);
        if (targetMol) {
            RDKit::MolOps::Kekulize(*targetMol);
        } else {
            throw ValueErrorException("");
        }
    } catch (const ValueErrorException &exc) {
        delete targetMol;
        delete mol;
        return;
    }

    RDKit::RWMol *scaffMol = NULL;
    RDKit::RWMol *targetScaffMol = NULL;

    // third and fourth parameter is used only by extended fingerprint and their
    // atoms are loaded. Scaffold morphs can contain atoms, which are not in
    // scaffold of "mol" or "targetMol", so the original molecules are always used.
    SimCoefCalculator scCalc(simCoeffSelector, fingerprintSelector, mol, targetMol);

    Fingerprint *targetFp = NULL;

    if (!scaff) {
        targetFp = scCalc.GetFingerprint(targetMol);
    } else {
        try {
            scaffMol = !candidate.scaffoldSmile.empty() ?
                RDKit::SmilesToMol(candidate.scaffoldSmile) :
                NULL;
            if (scaffMol) {
                RDKit::MolOps::Kekulize(*scaffMol);
            }
        } catch (const ValueErrorException &exc) {
            delete scaffMol;
            delete targetMol;
            delete mol;
            return;
        }

        try {
            targetScaffMol = !target.scaffoldSmile.empty() ?
                RDKit::SmilesToMol(target.scaffoldSmile) :
                NULL;
            if (targetScaffMol) {
                RDKit::MolOps::Kekulize(*targetScaffMol);
            }
        } catch (const ValueErrorException &exc) {
            delete targetScaffMol;
            delete scaffMol;
            delete targetMol;
            delete mol;
            return;
        }

        targetFp = targetScaffMol ?
            scCalc.GetFingerprint(targetScaffMol) : NULL;
    }

    std::vector<Fingerprint *> decoysFp;
    decoysFp.reserve(decoys.size());
    RDKit::RWMol *decoyMol = NULL;
    try {
        for (int i = 0; i < decoys.size(); ++i) {
            if (!scaff) {
                decoyMol = RDKit::SmilesToMol(decoys[i].smile);
            } else {
                if (decoys[i].scaffoldSmile.empty()) {
                    continue;
                }
                decoyMol = RDKit::SmilesToMol(decoys[i].scaffoldSmile);
            }
            if (decoyMol) {
                RDKit::MolOps::Kekulize(*decoyMol);
                decoysFp.push_back(scCalc.GetFingerprint(decoyMol));
                delete decoyMol;
                decoyMol = NULL;
            } else {
                throw ValueErrorException("");
            }
        }
    } catch (const ValueErrorException &exc) {
        REPORT_RECOVERY("Recovered from decoy kekulization failure.");
        for (int i = 0; i < decoysFp.size(); ++i) {
            delete decoysFp[i];
        }
        delete decoyMol;
        delete targetScaffMol;
        delete scaffMol;
        delete targetMol;
        delete mol;
        delete targetFp;
        return;
    }

    std::vector<MorphingStrategy *> strategies;
    InitStrategies(chemOperSelectors, strategies);

    RDKit::RWMol **newMols = new RDKit::RWMol *[morphAttempts];
    std::memset(newMols, 0, sizeof(RDKit::RWMol *) * morphAttempts);
    RDKit::RWMol **newScaffMols = new RDKit::RWMol *[morphAttempts];
    std::memset(newScaffMols, 0, sizeof(RDKit::RWMol *) * morphAttempts);
    ChemOperSelector *opers = new ChemOperSelector [morphAttempts];
    std::string *smiles = new std::string [morphAttempts];
    std::string *formulas = new std::string [morphAttempts];
    std::string *scaffSmiles = new std::string [morphAttempts];
    double *weights = new double [morphAttempts];
    double *sascores = new double [morphAttempts]; // added for SAScore
    double *distToTarget = new double [morphAttempts];
    double *distToClosestDecoy = new double [morphAttempts];

    ScaffoldSelector scaffSel = scaff ? scaff->GetSelector() : SF_NONE;

    // compute new morphs and smiles
    if (!tbbCtx.is_group_execution_cancelled()) {
        tbb::atomic<unsigned int> kekulizeFailureCount;
        tbb::atomic<unsigned int> sanitizeFailureCount;
        tbb::atomic<unsigned int> morphingFailureCount;
        kekulizeFailureCount = 0;
        sanitizeFailureCount = 0;
        morphingFailureCount = 0;
        try {
            MorphingData data(*mol, *targetMol, chemOperSelectors, scaffSel);

            CalculateMorphs calculateMorphs(
                data, strategies, opers, newMols, newScaffMols, smiles, formulas,
                scaffSmiles, scaff, weights, sascores, kekulizeFailureCount,
                sanitizeFailureCount, morphingFailureCount);

            tbb::parallel_for(tbb::blocked_range<int>(0, morphAttempts),
                calculateMorphs, tbb::auto_partitioner(), tbbCtx);
        } catch (const std::exception &exc) {
            REPORT_RECOVERY("Recovered from morphing data construction failure.");
        }
        if (kekulizeFailureCount > 0) {
            std::stringstream report;
            report << "Recovered from " << kekulizeFailureCount << " kekulization failures.";
            REPORT_RECOVERY(report.str());
        }
        if (sanitizeFailureCount > 0) {
            std::stringstream report;
            report << "Recovered from " << sanitizeFailureCount << " sanitization failures.";
            REPORT_RECOVERY(report.str());
        }
        if (morphingFailureCount > 0) {
            std::stringstream report;
            report << "Recovered from " << morphingFailureCount << " morphing failures.";
            REPORT_RECOVERY(report.str());
        }
    }

    // compute distances
    // we need to announce the decoy which we want to use
    if (!tbbCtx.is_group_execution_cancelled()) {
        CalculateDistances calculateDistances(!scaff ? newMols : newScaffMols,
            scCalc, targetFp, decoysFp, distToTarget, distToClosestDecoy,
            0/*candidate.nextDecoy*/);
        tbb::parallel_for(tbb::blocked_range<int>(0, morphAttempts),
            calculateDistances, tbb::auto_partitioner(), tbbCtx);
    }

    // return results
    if (!tbbCtx.is_group_execution_cancelled()) {
        ReturnResults returnResults(!scaff ? newMols : newScaffMols,
            smiles, formulas, candidate.smile, scaffSmiles, scaffSel,
            opers, weights, sascores, distToTarget, distToClosestDecoy,
            callerState, deliver);
        tbb::parallel_for(tbb::blocked_range<int>(0, morphAttempts),
            returnResults, tbb::auto_partitioner(), tbbCtx);
    }

    for (int i = 0; i < morphAttempts; ++i) {
        delete newMols[i];
        delete newScaffMols[i];
    }
    delete[] newMols;
    delete[] newScaffMols;
    delete[] opers;
    delete[] smiles;
    delete[] formulas;
    delete[] scaffSmiles;
    delete[] weights;
    delete[] sascores;
    delete[] distToTarget;
    delete[] distToClosestDecoy;

    delete mol;
    delete targetMol;
    delete scaffMol;
    delete targetScaffMol;
    delete targetFp;

    for (int i = 0; i < decoysFp.size(); ++i) {
        delete decoysFp[i];
    }

    for (int i = 0; i < strategies.size(); ++i) {
        delete strategies[i];
    }

}

/**
 * Modified function for the Activity Morphing
 */
void GenerateMorphs(
    MolpherMolecule &candidate,
    unsigned int morphAttempts,
//    FingerprintSelector fingerprintSelector,
//    SimCoeffSelector simCoeffSelector,
    std::vector<ChemOperSelector> &chemOperSelectors,
//    MolpherMolecule &target,
    std::vector<MolpherMolecule> &decoys,
    tbb::task_group_context &tbbCtx ,
    void *callerState,
    void (*deliver)(MolpherMolecule *, void *)
    , Scaffold* scaff
    )
{
    RDKit::RWMol *mol = NULL;
    try {
        mol = RDKit::SmilesToMol(candidate.smile);
        if (mol) {
            RDKit::MolOps::Kekulize(*mol);
        } else {
            throw ValueErrorException("");
        }
    } catch (const ValueErrorException &exc) {
        delete mol;
        return;
    }

    RDKit::RWMol *targetMol = NULL;
//    try {
//        targetMol = RDKit::SmilesToMol(target.smile);
//        if (targetMol) {
//            RDKit::MolOps::Kekulize(*targetMol);
//        } else {
//            throw ValueErrorException("");
//        }
//    } catch (const ValueErrorException &exc) {
//        delete targetMol;
//        delete mol;
//        return;
//    }

    RDKit::RWMol *scaffMol = NULL;
    RDKit::RWMol *targetScaffMol = NULL;

    // third and fourth parameter is used only by extended fingerprint and their
    // atoms are loaded. Scaffold morphs can contain atoms, which are not in
    // scaffold of "mol" or "targetMol", so the original molecules are always used.
//    SimCoefCalculator scCalc(simCoeffSelector, fingerprintSelector, mol, targetMol);

    Fingerprint *targetFp = NULL;

    if (!scaff) {
//        targetFp = scCalc.GetFingerprint(targetMol);
    } else {
//        try {
//            scaffMol = !candidate.scaffoldSmile.empty() ?
//                RDKit::SmilesToMol(candidate.scaffoldSmile) :
//                NULL;
//            if (scaffMol) {
//                RDKit::MolOps::Kekulize(*scaffMol);
//            }
//        } catch (const ValueErrorException &exc) {
//            delete scaffMol;
//            delete targetMol;
//            delete mol;
//            return;
//        }
//
//        try {
//            targetScaffMol = !target.scaffoldSmile.empty() ?
//                RDKit::SmilesToMol(target.scaffoldSmile) :
//                NULL;
//            if (targetScaffMol) {
//                RDKit::MolOps::Kekulize(*targetScaffMol);
//            }
//        } catch (const ValueErrorException &exc) {
//            delete targetScaffMol;
//            delete scaffMol;
//            delete targetMol;
//            delete mol;
//            return;
//        }
//
//        targetFp = targetScaffMol ?
//            scCalc.GetFingerprint(targetScaffMol) : NULL;
    }

    std::vector<Fingerprint *> decoysFp;
    decoysFp.reserve(decoys.size());
    RDKit::RWMol *decoyMol = NULL;
    try {
        for (int i = 0; i < decoys.size(); ++i) {
//            if (!scaff) {
//                decoyMol = RDKit::SmilesToMol(decoys[i].smile);
//            } else {
//                if (decoys[i].scaffoldSmile.empty()) {
//                    continue;
//                }
//                decoyMol = RDKit::SmilesToMol(decoys[i].scaffoldSmile);
//            }
//            if (decoyMol) {
//                RDKit::MolOps::Kekulize(*decoyMol);
//                decoysFp.push_back(scCalc.GetFingerprint(decoyMol));
//                delete decoyMol;
//                decoyMol = NULL;
//            } else {
//                throw ValueErrorException("");
//            }
        }
    } catch (const ValueErrorException &exc) {
        REPORT_RECOVERY("Recovered from decoy kekulization failure.");
        for (int i = 0; i < decoysFp.size(); ++i) {
            delete decoysFp[i];
        }
        delete decoyMol;
        delete targetScaffMol;
        delete scaffMol;
//        delete targetMol;
        delete mol;
        delete targetFp;
        return;
    }

    std::vector<MorphingStrategy *> strategies;
    InitStrategies(chemOperSelectors, strategies);

    RDKit::RWMol **newMols = new RDKit::RWMol *[morphAttempts];
    std::memset(newMols, 0, sizeof(RDKit::RWMol *) * morphAttempts);
    RDKit::RWMol **newScaffMols = new RDKit::RWMol *[morphAttempts];
    std::memset(newScaffMols, 0, sizeof(RDKit::RWMol *) * morphAttempts);
    ChemOperSelector *opers = new ChemOperSelector [morphAttempts];
    std::string *smiles = new std::string [morphAttempts];
    std::string *formulas = new std::string [morphAttempts];
    std::string *scaffSmiles = new std::string [morphAttempts];
    double *weights = new double [morphAttempts];
    double *sascores = new double [morphAttempts]; // added for SAScore
    double *distToTarget = new double [morphAttempts];
    double *distToClosestDecoy = new double [morphAttempts];

    ScaffoldSelector scaffSel = scaff ? scaff->GetSelector() : SF_NONE;

    // compute new morphs and smiles
    if (!tbbCtx.is_group_execution_cancelled()) {
        tbb::atomic<unsigned int> kekulizeFailureCount;
        tbb::atomic<unsigned int> sanitizeFailureCount;
        tbb::atomic<unsigned int> morphingFailureCount;
        kekulizeFailureCount = 0;
        sanitizeFailureCount = 0;
        morphingFailureCount = 0;
        try {
            // TODO: ask about the purpose of sending in second molecule (in original version ve sent in target)
            MorphingData data(*mol, *mol, chemOperSelectors, scaffSel); 
            
            CalculateMorphs calculateMorphs(
                data, strategies, opers, newMols, newScaffMols, smiles, formulas,
                scaffSmiles, scaff, weights, sascores, kekulizeFailureCount,
                sanitizeFailureCount, morphingFailureCount);

            tbb::parallel_for(tbb::blocked_range<int>(0, morphAttempts),
                calculateMorphs, tbb::auto_partitioner(), tbbCtx);
        } catch (const std::exception &exc) {
            REPORT_RECOVERY("Recovered from morphing data construction failure.");
        }
        if (kekulizeFailureCount > 0) {
            std::stringstream report;
            report << "Recovered from " << kekulizeFailureCount << " kekulization failures.";
            REPORT_RECOVERY(report.str());
        }
        if (sanitizeFailureCount > 0) {
            std::stringstream report;
            report << "Recovered from " << sanitizeFailureCount << " sanitization failures.";
            REPORT_RECOVERY(report.str());
        }
        if (morphingFailureCount > 0) {
            std::stringstream report;
            report << "Recovered from " << morphingFailureCount << " morphing failures.";
            REPORT_RECOVERY(report.str());
        }
    }

    // compute distances
    // we need to announce the decoy which we want to use
//    if (!tbbCtx.is_group_execution_cancelled()) {
//        CalculateDistances calculateDistances(!scaff ? newMols : newScaffMols,
//            scCalc, targetFp, decoysFp, distToTarget, distToClosestDecoy,
//            0/*candidate.nextDecoy*/);
//        tbb::parallel_for(tbb::blocked_range<int>(0, morphAttempts),
//            calculateDistances, tbb::auto_partitioner(), tbbCtx);
//    }

    // return results
    if (!tbbCtx.is_group_execution_cancelled()) {
        ReturnResults returnResults(!scaff ? newMols : newScaffMols,
            smiles, formulas, candidate.smile, scaffSmiles, scaffSel,
            opers, weights, sascores, distToTarget, distToClosestDecoy,
            callerState, deliver);
        tbb::parallel_for(tbb::blocked_range<int>(0, morphAttempts),
            returnResults, tbb::auto_partitioner(), tbbCtx);
    }

    for (int i = 0; i < morphAttempts; ++i) {
        delete newMols[i];
        delete newScaffMols[i];
    }
    delete[] newMols;
    delete[] newScaffMols;
    delete[] opers;
    delete[] smiles;
    delete[] formulas;
    delete[] scaffSmiles;
    delete[] weights;
    delete[] sascores;
    delete[] distToTarget;
    delete[] distToClosestDecoy;

    delete mol;
    delete targetMol;
    delete scaffMol;
    delete targetScaffMol;
    delete targetFp;

    for (int i = 0; i < decoysFp.size(); ++i) {
        delete decoysFp[i];
    }

    for (int i = 0; i < strategies.size(); ++i) {
        delete strategies[i];
    }

}
