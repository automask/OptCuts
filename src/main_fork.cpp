#include <igl/arap.h>
#include <igl/avg_edge_length.h>
#include <igl/boundary_loop.h>
#include <igl/cut_mesh.h>
#include <igl/edge_lengths.h>
#include <igl/euler_characteristic.h>
#include <igl/facet_components.h>
#include <igl/harmonic.h>
#include <igl/is_edge_manifold.h>
#include <igl/is_vertex_manifold.h>
#include <igl/map_vertices_to_circle.h>
#include <igl/png/writePNG.h>
#include <igl/readOBJ.h>
#include <igl/readOFF.h>
#include <igl/writeOBJ.h>
#include <sys/stat.h>  // for mkdir

#include <ctime>
#include <fstream>
#include <string>

#include "IglUtils.hpp"
#include "Optimizer.hpp"
#include "SymDirichletEnergy.hpp"
#include "Timer.hpp"
#include "Types.hpp"
#include "cut_to_disk.hpp"  // hasn't been pulled into the older version of libigl we use

#if defined _MSC_VER
#include <direct.h>

#define mkdir(dir, mode) _mkdir(dir)
#endif

Eigen::MatrixXd V, UV, N;
Eigen::MatrixXi F, FUV, FN;

// optimization
OptCuts::MethodType methodType;
std::vector<const OptCuts::TriMesh*> triSoup;
int vertAmt_input;
OptCuts::TriMesh triSoup_backup;
OptCuts::Optimizer* optimizer;
std::vector<OptCuts::Energy*> energyTerms;
std::vector<double> energyParams;

bool bijectiveParam = true;
bool rand1PInitCut = false;
double lambda_init;
bool optimization_on = false;

int optimization_on_times = 0;
int iter_num_zero = 0;

int iterNum = 0;
int converged = 0;
bool fractureMode = false;
double fracThres = 0.0;
bool topoLineSearch = true;
int initCutOption = 0;
bool outerLoopFinished = false;
double upperBound = 4.1;
const double convTol_upperBound = 1.0e-3;

std::vector<std::pair<double, double>> energyChanges_bSplit, energyChanges_iSplit, energyChanges_merge;
std::vector<std::vector<int>> paths_bSplit, paths_iSplit, paths_merge;
std::vector<Eigen::MatrixXd> newVertPoses_bSplit, newVertPoses_iSplit, newVertPoses_merge;

int opType_queried = -1;
std::vector<int> path_queried;
Eigen::MatrixXd newVertPos_queried;
bool reQuery = false;
double filterExp_in = 0.6;
int inSplitTotalAmt;

std::ofstream logFile;
std::string outputFolderPath = "output/";

// visualization
bool headlessMode = false;
const int channel_initial = 0;
const int channel_result = 1;
const int channel_findExtrema = 2;
int viewChannel = channel_result;
bool viewUV = true;  // view UV or 3D model
double texScale = 1.0;
bool showSeam = true;
Eigen::MatrixXd seamColor;
int showDistortion = 1;   // 0: don't show; 1: SD energy value; 2: L2 stretch value;
bool showTexture = true;  // show checkerboard
bool isLighting = false;
bool showFracTail = true;
float fracTailSize = 15.0f;
bool offlineMode = false;
std::string infoName = "";

double secPast = 0.0;
time_t lastStart_world;
Timer timer, timer_step;

void saveInfoForPresent(const std::string fileName = "info.txt")
{
    std::ofstream file;
    file.open(outputFolderPath + fileName);
    assert(file.is_open());

    file << vertAmt_input << " " << triSoup[channel_initial]->F.rows() << std::endl;

    file << iterNum << " " << optimizer->getTopoIter() << " 0 0 " << lambda_init << " " << 1.0 - energyParams[0] << std::endl;

    file << "0.0 0.0 " << timer.timing_total() << " " << secPast << " topo" << timer.timing(0) << " desc" << timer.timing(1) << " scaf"
         << timer.timing(2) << " enUp" << timer.timing(3) << " mtrComp" << timer_step.timing(0) << " mtrAssem" << timer_step.timing(1) << " symFac"
         << timer_step.timing(2) << " numFac" << timer_step.timing(3) << " backSolve" << timer_step.timing(4) << " lineSearch" << timer_step.timing(5)
         << " bSplit" << timer_step.timing(6) << " iSplit" << timer_step.timing(7) << " cMerge" << timer_step.timing(8) << std::endl;

    double seamLen;
    if (energyParams[0] == 1.0)
    {
        // pure distortion minimization mode for models with initial cuts also reflected on the surface as boundary edges...
        triSoup[channel_result]->computeBoundaryLen(seamLen);
        seamLen /= 2.0;
    }
    else
    {
        triSoup[channel_result]->computeSeamSparsity(seamLen, !fractureMode);
    }
    double distortion;
    energyTerms[0]->computeEnergyVal(*triSoup[channel_result], distortion);
    file << distortion << " " << seamLen / triSoup[channel_result]->virtualRadius << std::endl;

    triSoup[channel_result]->outputStandardStretch(file);

    file << "initialSeams " << triSoup[channel_result]->initSeams.rows() << std::endl;
    file << triSoup[channel_result]->initSeams << std::endl;

    file.close();
}

void proceedOptimization(int proceedNum = 1)
{
    for (int proceedI = 0; (proceedI < proceedNum) && (!converged); proceedI++)
    {
        // 迭代
        std::cout << "-- Iteration" << iterNum << ":" << std::endl;
        // 优化求解|收敛
        converged = optimizer->solve(1);
        iterNum = optimizer->getIterNum();
    }
}

int computeOptPicked(const std::vector<std::pair<double, double>>& energyChanges0, const std::vector<std::pair<double, double>>& energyChanges1,
                     double lambda)
{
    assert(!energyChanges0.empty());
    assert(!energyChanges1.empty());
    assert((lambda >= 0.0) && (lambda <= 1.0));

    double minEChange0 = __DBL_MAX__;
    for (int ecI = 0; ecI < energyChanges0.size(); ecI++)
    {
        if ((energyChanges0[ecI].first == __DBL_MAX__) || (energyChanges0[ecI].second == __DBL_MAX__))
        {
            continue;
        }
        double EwChange = energyChanges0[ecI].first * (1.0 - lambda) + energyChanges0[ecI].second * lambda;
        if (EwChange < minEChange0)
        {
            minEChange0 = EwChange;
        }
    }

    double minEChange1 = __DBL_MAX__;
    for (int ecI = 0; ecI < energyChanges1.size(); ecI++)
    {
        if ((energyChanges1[ecI].first == __DBL_MAX__) || (energyChanges1[ecI].second == __DBL_MAX__))
        {
            continue;
        }
        double EwChange = energyChanges1[ecI].first * (1.0 - lambda) + energyChanges1[ecI].second * lambda;
        if (EwChange < minEChange1)
        {
            minEChange1 = EwChange;
        }
    }

    assert((minEChange0 != __DBL_MAX__) || (minEChange1 != __DBL_MAX__));
    return (minEChange0 > minEChange1);
}

int computeBestCand(const std::vector<std::pair<double, double>>& energyChanges, double lambda, double& bestEChange)
{
    assert((lambda >= 0.0) && (lambda <= 1.0));

    bestEChange = __DBL_MAX__;
    int id_minEChange = -1;
    for (int ecI = 0; ecI < energyChanges.size(); ecI++)
    {
        if ((energyChanges[ecI].first == __DBL_MAX__) || (energyChanges[ecI].second == __DBL_MAX__))
        {
            continue;
        }
        double EwChange = energyChanges[ecI].first * (1.0 - lambda) + energyChanges[ecI].second * lambda;
        if (EwChange < bestEChange)
        {
            bestEChange = EwChange;
            id_minEChange = ecI;
        }
    }

    return id_minEChange;
}

bool checkCand(const std::vector<std::pair<double, double>>& energyChanges)
{
    for (const auto& candI : energyChanges)
    {
        if ((candI.first < 0.0) || (candI.second < 0.0))
        {
            return true;
        }
    }

    double minEChange = __DBL_MAX__;
    for (const auto& candI : energyChanges)
    {
        if (candI.first < minEChange)
        {
            minEChange = candI.first;
        }
        if (candI.second < minEChange)
        {
            minEChange = candI.second;
        }
    }
    std::cout << "candidates not valid, minEChange: " << minEChange << std::endl;
    return false;
}

double updateLambda(double measure_bound, double lambda_SD = energyParams[0], double kappa = 1.0, double kappa2 = 1.0)
{
    lambda_SD = std::max(0.0, kappa * (measure_bound - (upperBound - convTol_upperBound / 2.0)) + kappa2 * lambda_SD / (1.0 - lambda_SD));
    return lambda_SD / (1.0 + lambda_SD);
}

bool updateLambda_stationaryV(bool cancelMomentum = true, bool checkConvergence = false)
{
    Eigen::MatrixXd edgeLengths;
    igl::edge_lengths(triSoup[channel_result]->V_rest, triSoup[channel_result]->F, edgeLengths);
    const double eps_E_se = 1.0e-3 * edgeLengths.minCoeff() / triSoup[channel_result]->virtualRadius;

    // measurement and energy value computation
    const double E_SD = optimizer->getLastEnergyVal(true) / energyParams[0];
    double E_se;
    triSoup[channel_result]->computeSeamSparsity(E_se);
    E_se /= triSoup[channel_result]->virtualRadius;
    double stretch_l2, stretch_inf, stretch_shear, compress_inf;
    triSoup[channel_result]->computeStandardStretch(stretch_l2, stretch_inf, stretch_shear, compress_inf);
    double measure_bound = E_SD;
    const double eps_lambda = std::min(1.0e-3, std::abs(updateLambda(measure_bound) - energyParams[0]));

    // TODO?: stop when first violates bounds from feasible, don't go to best feasible. check after each merge whether distortion is violated
    //  oscillation detection
    static int iterNum_bestFeasible = -1;
    static OptCuts::TriMesh triSoup_bestFeasible;
    static double E_se_bestFeasible = __DBL_MAX__;
    static int lastStationaryIterNum = 0;  // still necessary because boundary and interior query are with same iterNum
    static std::map<double, std::vector<std::pair<double, double>>> configs_stationaryV;
    if (iterNum != lastStationaryIterNum)
    {
        // not a roll back config
        const double lambda = 1.0 - energyParams[0];
        bool oscillate = false;
        const auto low = configs_stationaryV.lower_bound(E_se);
        if (low == configs_stationaryV.end())
        {
            // all less than E_se
            if (!configs_stationaryV.empty())
            {
                // use largest element
                if (std::abs(configs_stationaryV.rbegin()->first - E_se) < eps_E_se)
                {
                    for (const auto& lambdaI : configs_stationaryV.rbegin()->second)
                    {
                        if ((std::abs(lambdaI.first - lambda) < eps_lambda) && (std::abs(lambdaI.second - E_SD) < eps_E_se))
                        {
                            oscillate = true;
                            logFile << configs_stationaryV.rbegin()->first << ", " << lambdaI.second << std::endl;
                            logFile << E_se << ", " << lambda << ", " << E_SD << std::endl;
                            break;
                        }
                    }
                }
            }
        }
        else if (low == configs_stationaryV.begin())
        {
            // all not less than E_se
            if (std::abs(low->first - E_se) < eps_E_se)
            {
                for (const auto& lambdaI : low->second)
                {
                    if ((std::abs(lambdaI.first - lambda) < eps_lambda) && (std::abs(lambdaI.second - E_SD) < eps_E_se))
                    {
                        oscillate = true;
                        logFile << low->first << ", " << lambdaI.first << ", " << lambdaI.second << std::endl;
                        logFile << E_se << ", " << lambda << ", " << E_SD << std::endl;
                        break;
                    }
                }
            }
        }
        else
        {
            const auto prev = std::prev(low);
            if (std::abs(low->first - E_se) < eps_E_se)
            {
                for (const auto& lambdaI : low->second)
                {
                    if ((std::abs(lambdaI.first - lambda) < eps_lambda) && (std::abs(lambdaI.second - E_SD) < eps_E_se))
                    {
                        oscillate = true;
                        logFile << low->first << ", " << lambdaI.first << ", " << lambdaI.second << std::endl;
                        logFile << E_se << ", " << lambda << ", " << E_SD << std::endl;
                        break;
                    }
                }
            }
            if ((!oscillate) && (std::abs(prev->first - E_se) < eps_E_se))
            {
                for (const auto& lambdaI : prev->second)
                {
                    if ((std::abs(lambdaI.first - lambda) < eps_lambda) && (std::abs(lambdaI.second - E_SD) < eps_E_se))
                    {
                        oscillate = true;
                        logFile << prev->first << ", " << lambdaI.first << ", " << lambdaI.second << std::endl;
                        logFile << E_se << ", " << lambda << ", " << E_SD << std::endl;
                        break;
                    }
                }
            }
        }

        // record best feasible UV map
        if ((measure_bound <= upperBound) && (E_se < E_se_bestFeasible))
        {
            iterNum_bestFeasible = iterNum;
            triSoup_bestFeasible = *triSoup[channel_result];
            E_se_bestFeasible = E_se;
        }

        if (oscillate && (iterNum_bestFeasible >= 0))
        {
            // arrive at the best feasible config again
            logFile << "oscillation detected at measure = " << measure_bound << ", b = " << upperBound << "lambda = " << energyParams[0] << std::endl;
            logFile << lastStationaryIterNum << ", " << iterNum << std::endl;
            if (iterNum_bestFeasible != iterNum)
            {
                optimizer->setConfig(triSoup_bestFeasible, iterNum, optimizer->getTopoIter());
                logFile << "rolled back to best feasible in iter " << iterNum_bestFeasible << std::endl;
            }
            return false;
        }
        else
        {
            configs_stationaryV[E_se].emplace_back(std::pair<double, double>(lambda, E_SD));
        }
    }
    lastStationaryIterNum = iterNum;

    // convergence check
    if (checkConvergence)
    {
        if (measure_bound <= upperBound)
        {
            // save info at first feasible stationaryVT for comparison
            static bool saved = false;
            if (!saved)
            {
                //                logFile << "saving firstFeasibleS..." << std::endl;
                //                triSoup[channel_result]->saveAsMesh(outputFolderPath + "firstFeasibleS_mesh.obj", F);
                secPast += difftime(time(NULL), lastStart_world);
                time(&lastStart_world);
                saved = true;
                //                logFile << "firstFeasibleS saved" << std::endl;
            }

            if (measure_bound >= upperBound - convTol_upperBound)
            {
                logFile << "all converged at measure = " << measure_bound << ", b = " << upperBound << " lambda = " << energyParams[0] << std::endl;
                if (iterNum_bestFeasible != iterNum)
                {
                    assert(iterNum_bestFeasible >= 0);
                    optimizer->setConfig(triSoup_bestFeasible, iterNum, optimizer->getTopoIter());
                    logFile << "rolled back to best feasible in iter " << iterNum_bestFeasible << std::endl;
                }
                return false;
            }
        }
    }

    // lambda update (dual update)
    energyParams[0] = updateLambda(measure_bound);
    // TODO: needs to be careful on lambda update space

    // critical lambda scheme
    if (checkConvergence)
    {
        // update lambda until feasible update on T might be triggered
        if (measure_bound > upperBound)
        {
            // need to cut further, increase energyParams[0]
            logFile << "curUpdated = " << energyParams[0] << ", increase" << std::endl;

            if ((!energyChanges_merge.empty()) && (computeOptPicked(energyChanges_bSplit, energyChanges_merge, 1.0 - energyParams[0]) == 1))
            {
                // still picking merge
                do
                {
                    energyParams[0] = updateLambda(measure_bound);
                } while ((computeOptPicked(energyChanges_bSplit, energyChanges_merge, 1.0 - energyParams[0]) == 1));

                logFile << "iterativelyUpdated = " << energyParams[0] << ", increase for switch" << std::endl;
            }

            if ((!checkCand(energyChanges_iSplit)) && (!checkCand(energyChanges_bSplit)))
            {
                // if filtering too strong
                reQuery = true;
                logFile << "enlarge filtering!" << std::endl;
            }
            else
            {
                double eDec_b, eDec_i;
                assert(!(energyChanges_bSplit.empty() && energyChanges_iSplit.empty()));
                int id_pickingBSplit = computeBestCand(energyChanges_bSplit, 1.0 - energyParams[0], eDec_b);
                int id_pickingISplit = computeBestCand(energyChanges_iSplit, 1.0 - energyParams[0], eDec_i);
                while ((eDec_b > 0.0) && (eDec_i > 0.0))
                {
                    energyParams[0] = updateLambda(measure_bound);
                    id_pickingBSplit = computeBestCand(energyChanges_bSplit, 1.0 - energyParams[0], eDec_b);
                    id_pickingISplit = computeBestCand(energyChanges_iSplit, 1.0 - energyParams[0], eDec_i);
                }
                if (eDec_b <= 0.0)
                {
                    opType_queried = 0;
                    path_queried = paths_bSplit[id_pickingBSplit];
                    newVertPos_queried = newVertPoses_bSplit[id_pickingBSplit];
                }
                else
                {
                    opType_queried = 1;
                    path_queried = paths_iSplit[id_pickingISplit];
                    newVertPos_queried = newVertPoses_iSplit[id_pickingISplit];
                }

                logFile << "iterativelyUpdated = " << energyParams[0] << ", increased, current eDec = " << eDec_b << ", " << eDec_i
                        << "; id: " << id_pickingBSplit << ", " << id_pickingISplit << std::endl;
            }
        }
        else
        {
            bool noOp = true;
            for (const auto ecI : energyChanges_merge)
            {
                if (ecI.first != __DBL_MAX__)
                {
                    noOp = false;
                    break;
                }
            }
            if (noOp)
            {
                logFile << "No merge operation available, end process!" << std::endl;
                energyParams[0] = 1.0 - eps_lambda;
                optimizer->updateEnergyData(true, false, false);
                if (iterNum_bestFeasible != iterNum)
                {
                    optimizer->setConfig(triSoup_bestFeasible, iterNum, optimizer->getTopoIter());
                }
                return false;
            }

            logFile << "curUpdated = " << energyParams[0] << ", decrease" << std::endl;

            //!!! also account for iSplit for this switch?
            if (computeOptPicked(energyChanges_bSplit, energyChanges_merge, 1.0 - energyParams[0]) == 0)
            {
                // still picking split
                do
                {
                    energyParams[0] = updateLambda(measure_bound);
                } while (computeOptPicked(energyChanges_bSplit, energyChanges_merge, 1.0 - energyParams[0]) == 0);

                logFile << "iterativelyUpdated = " << energyParams[0] << ", decrease for switch" << std::endl;
            }

            double eDec_m;
            assert(!energyChanges_merge.empty());
            int id_pickingMerge = computeBestCand(energyChanges_merge, 1.0 - energyParams[0], eDec_m);
            while (eDec_m > 0.0)
            {
                energyParams[0] = updateLambda(measure_bound);
                id_pickingMerge = computeBestCand(energyChanges_merge, 1.0 - energyParams[0], eDec_m);
            }
            opType_queried = 2;
            path_queried = paths_merge[id_pickingMerge];
            newVertPos_queried = newVertPoses_merge[id_pickingMerge];

            logFile << "iterativelyUpdated = " << energyParams[0] << ", decreased, current eDec = " << eDec_m << std::endl;
        }
    }

    // lambda value sanity check
    if (energyParams[0] > 1.0 - eps_lambda)
    {
        energyParams[0] = 1.0 - eps_lambda;
    }
    if (energyParams[0] < eps_lambda)
    {
        energyParams[0] = eps_lambda;
    }

    optimizer->updateEnergyData(true, false, false);

    logFile << "measure = " << measure_bound << ", b = " << upperBound << ", updated lambda = " << energyParams[0] << std::endl;
    return true;
}

void converge_preDrawFunc()
{
    infoName = "finalResult";

    if (!bijectiveParam)
    {
        // perform exact solve
        optimizer->setAllowEDecRelTol(false);
        converged = false;
        optimizer->setPropagateFracture(false);

        while (!converged)
        {
            proceedOptimization(1000);
        }
    }

    secPast += difftime(time(NULL), lastStart_world);

    optimizer->flushEnergyFileOutput();
    optimizer->flushGradFileOutput();

    optimization_on = false;

    std::cout << "optimization converged, with " << secPast << "s." << std::endl;
    logFile << "optimization converged, with " << secPast << "s." << std::endl;
    outerLoopFinished = true;  // 预告结束
}

void preDrawFunc()
{
    if (!optimization_on) return;

    optimization_on_times++;
    // 收敛|converged
    while (!converged)
    {
        proceedOptimization();
    }

    if (!converged) return;

    double stretch_l2, stretch_inf, stretch_shear, compress_inf;
    triSoup[channel_result]->computeStandardStretch(stretch_l2, stretch_inf, stretch_shear, compress_inf);
    double measure_bound = optimizer->getLastEnergyVal(true) / energyParams[0];

    // case OptCuts::MT_OPTCUTS:
    infoName = std::to_string(iterNum);
    if (converged == 2)
    {
        converged = 0;
        return;
    }

    if (measure_bound <= upperBound)
    {
        // save info once bound is reached for comparison
        static bool saved = false;
        if (!saved)
        {
            //                            triSoup[channel_result]->save(outputFolderPath + infoName + "_triSoup.obj");
            //                            triSoup[channel_result]->saveAsMesh(outputFolderPath + "firstFeasible_mesh.obj", F);
            secPast += difftime(time(NULL), lastStart_world);
            time(&lastStart_world);
            saved = true;
        }
    }

    // if necessary, turn on scaffolding for random one point initial cut
    if (!optimizer->isScaffolding() && bijectiveParam && rand1PInitCut)
    {
        optimizer->setScaffolding(true);
    }

    double E_se;
    triSoup[channel_result]->computeSeamSparsity(E_se);
    E_se /= triSoup[channel_result]->virtualRadius;
    const double E_SD = optimizer->getLastEnergyVal(true) / energyParams[0];

    std::cout << iterNum << ": " << E_SD << " " << E_se << " " << triSoup[channel_result]->V_rest.rows() << std::endl;
    logFile << iterNum << ": " << E_SD << " " << E_se << " " << triSoup[channel_result]->V_rest.rows() << std::endl;
    optimizer->flushEnergyFileOutput();
    optimizer->flushGradFileOutput();

    // continue to split boundary
    if (!updateLambda_stationaryV())
    {
        // oscillation detected
        converge_preDrawFunc();
        return;
    }

    logFile << "boundary op V " << triSoup[channel_result]->V_rest.rows() << std::endl;
    if (optimizer->createFracture(fracThres, false, topoLineSearch))
    {
        converged = false;
        return;
    }

    // if no boundary op, try interior split if split is the current best boundary op
    if ((measure_bound > upperBound) && optimizer->createFracture(fracThres, false, topoLineSearch, true))
    {
        logFile << "interior split " << triSoup[channel_result]->V_rest.rows() << std::endl;
        converged = false;
        return;
    }

    if (!updateLambda_stationaryV(false, true))
    {
        // all converged
        converge_preDrawFunc();
        return;
    }

    // split or merge after lambda update
    if (reQuery)
    {
        filterExp_in += std::log(2.0) / std::log(inSplitTotalAmt);
        filterExp_in = std::min(1.0, filterExp_in);
        while (!optimizer->createFracture(fracThres, false, topoLineSearch, true))
        {
            filterExp_in += std::log(2.0) / std::log(inSplitTotalAmt);
            filterExp_in = std::min(1.0, filterExp_in);
        }
        reQuery = false;
        // TODO: set filtering param back?
    }
    else
    {
        optimizer->createFracture(opType_queried, path_queried, newVertPos_queried, topoLineSearch);
    }

    opType_queried = -1;
    converged = false;
}

void postDrawFunc()
{
    if (iterNum == 0)
    {
        iter_num_zero++;

        if (optimization_on)
        {
            optimization_on = false;
            return;
        }

        if (!optimization_on && !converged)
        {
            optimization_on = true;
            return;
        }
    }

    if (!outerLoopFinished) return;

    // 保存模型
    triSoup[channel_result]->saveAsMesh(outputFolderPath + infoName + "_mesh.obj", F);
    triSoup[channel_result]->saveAsMesh(outputFolderPath + infoName + "_mesh_normalizedUV.obj", F, true);

    // 保存结果
    saveInfoForPresent();
    std::cout << ">>> optimization on times " << optimization_on_times << std::endl;
    std::cout << ">>> iter num zero times " << iter_num_zero << std::endl;

    // 退出
    exit(0);
}

int main(int argc, char* argv[])
{
    // headless mode
    offlineMode = true;
    headlessMode = true;
    std::cout << "Headless mode" << std::endl;

    // Optimization mode
    mkdir(outputFolderPath.c_str(), 0777);

    std::string meshFileName("cone2.0.obj");
    if (argc > 2)
    {
        meshFileName = std::string(argv[2]);
    }
    std::string meshFilePath = meshFileName;
    meshFileName = meshFileName.substr(meshFileName.find_last_of('/') + 1);

    std::string meshFolderPath = meshFilePath.substr(0, meshFilePath.find_last_of('/'));
    std::string meshName = meshFileName.substr(0, meshFileName.find_last_of('.'));

    // Load mesh
    const std::string suffix = meshFilePath.substr(meshFilePath.find_last_of('.'));
    bool loadSucceed = false;
    if (suffix == ".off")
    {
        loadSucceed = igl::readOFF(meshFilePath, V, F);
    }
    else if (suffix == ".obj")
    {
        // 输入数据
        loadSucceed = igl::readOBJ(meshFilePath, V, UV, N, F, FUV, FN);
    }
    else
    {
        std::cout << "unkown mesh file format!" << std::endl;
        return -1;
    }

    if (!loadSucceed)
    {
        std::cout << "failed to load mesh!" << std::endl;
        return -1;
    }
    vertAmt_input = V.rows();

    Eigen::VectorXi B;
    bool isManifold = igl::is_vertex_manifold(F, B) && igl::is_edge_manifold(F);
    if (!isManifold)
    {
        std::cout << "input mesh contains non-manifold edges or vertices" << std::endl;
        std::cout << "please cleanup the mesh and retry" << std::endl;
        exit(-1);
    }

    // Set lambda
    lambda_init = 0.999;
    if (argc > 3)
    {
        lambda_init = std::stod(argv[3]);
        if ((lambda_init != lambda_init) || (lambda_init < 0.0) || (lambda_init >= 1.0))
        {
            std::cout << "Overwrite invalid lambda " << lambda_init << " to 0.999" << std::endl;
            lambda_init = 0.999;
        }
    }
    else
    {
        std::cout << "Use default lambda = " << lambda_init << std::endl;
    }

    // Set testID
    double testID = 1.0;  // test id for naming result folder
    if (argc > 4)
    {
        testID = std::stod(argv[4]);
        if ((testID != testID) || (testID < 0.0))
        {
            std::cout << "Overwrite invalid testID " << testID << " to 1" << std::endl;
            testID = 1.0;
        }
    }
    else
    {
        std::cout << "Use default testID = " << testID << std::endl;
    }

    std::string startDS;

    // extern 访问|argv[5]
    methodType = OptCuts::MT_OPTCUTS;
    // switch (methodType)
    // case OptCuts::MT_OPTCUTS:
    {
        startDS = "OptCuts";
        std::cout << ">>> method: OptCuts." << std::endl;
    }

    if (argc > 6)
    {
        upperBound = std::stod(argv[6]);
        if (upperBound == 0.0)
        {
            // read in b_d for comparing to other methods
            bool useScriptedBound = false;
            std::ifstream distFile(outputFolderPath + "distortion.txt");
            assert(distFile.is_open());

            std::string resultName;
            double resultDistortion;
            while (!distFile.eof())
            {
                distFile >> resultName >> resultDistortion;
                if ((resultName.find(meshName + "_Tutte_") != std::string::npos) || (resultName.find(meshName + "_input_") != std::string::npos) ||
                    (resultName.find(meshName + "_HighGenus_") != std::string::npos) ||
                    (resultName.find(meshName + "_rigid_") != std::string::npos) || (resultName.find(meshName + "_zbrush_") != std::string::npos) ||
                    (resultName.find(meshName + "_unwrella_") != std::string::npos))
                {
                    useScriptedBound = true;
                    upperBound = resultDistortion;
                    assert(upperBound > 4.0);
                    break;
                }
            }
            distFile.close();

            assert(useScriptedBound);
            std::cout << "Use scripted b_d = " << upperBound << std::endl;
        }
        else
        {
            if (upperBound <= 4.0)
            {
                std::cout << "input b_d <= 4.0! use 4.1 instead." << std::endl;
                upperBound = 4.1;
            }
            else
            {
                std::cout << "use b_d = " << upperBound << std::endl;
            }
        }
    }

    if (argc > 7)
    {
        bijectiveParam = std::stoi(argv[7]);
        std::cout << "bijectivity " << (bijectiveParam ? "ON" : "OFF") << std::endl;
    }

    if (argc > 8)
    {
        initCutOption = std::stoi(argv[8]);
    }
    switch (initCutOption)
    {
        case 0:
            std::cout << "random 2-edge initial cut for genus-0 closed surface" << std::endl;
            break;

        case 1:
            std::cout << "farthest 2-point initial cut for genus-0 closed surface" << std::endl;
            break;

        default:
            std::cout << "input initial cut option invalid, use default" << std::endl;
            std::cout << "random 2-edge initial cut for genus-0 closed surface" << std::endl;
            initCutOption = 0;
            break;
    }

    std::string folderTail = "";
    if (argc > 9)
    {
        if (argv[9][0] != '_')
        {
            folderTail += '_';
        }
        folderTail += argv[9];
    }

    //////////////////////////////////
    // initialize UV

    if (UV.rows() != 0)
    {
        // with input UV
        // 构造输入 Mesh|自带UV
        OptCuts::TriMesh* temp = new OptCuts::TriMesh(V, F, UV, FUV, false);

        std::vector<std::vector<int>> bnd_all;
        igl::boundary_loop(temp->F, bnd_all);

        // TODO: check input UV genus (validity)
        // right now OptCuts assumes input UV is a set of topological disks

        bool recompute_UV_needed = !temp->checkInversion();
        if ((!recompute_UV_needed) && bijectiveParam && (bnd_all.size() > 1))
        {
            // TODO: check overlaps and decide whether needs recompute UV
            // needs to check even if bnd_all.size() == 1
            // right now OptCuts take the input seams and recompute UV by default when bijective mapping is enabled
            recompute_UV_needed = true;
        }
        if (recompute_UV_needed)
        {
            std::cout << "local injectivity violated in given input UV map, "
                      << "or multi-chart bijective UV map needs to be ensured, "
                      << "obtaining new initial UV map by applying Tutte's embedding..." << std::endl;

            int UVGridDim = 0;
            do
            {
                ++UVGridDim;
            } while (UVGridDim * UVGridDim < bnd_all.size());
            std::cout << "UVGridDim " << UVGridDim << std::endl;

            Eigen::VectorXi bnd_stacked;
            Eigen::MatrixXd bnd_uv_stacked;
            for (int bndI = 0; bndI < bnd_all.size(); bndI++)
            {
                // map boundary to unit circle
                bnd_stacked.conservativeResize(bnd_stacked.size() + bnd_all[bndI].size());
                bnd_stacked.tail(bnd_all[bndI].size()) = Eigen::VectorXi::Map(bnd_all[bndI].data(), bnd_all[bndI].size());

                Eigen::MatrixXd bnd_uv;
                igl::map_vertices_to_circle(temp->V_rest, bnd_stacked.tail(bnd_all[bndI].size()), bnd_uv);
                double xOffset = bndI % UVGridDim * 2.1, yOffset = bndI / UVGridDim * 2.1;
                for (int bnd_uvI = 0; bnd_uvI < bnd_uv.rows(); bnd_uvI++)
                {
                    bnd_uv(bnd_uvI, 0) += xOffset;
                    bnd_uv(bnd_uvI, 1) += yOffset;
                }
                bnd_uv_stacked.conservativeResize(bnd_uv_stacked.rows() + bnd_uv.rows(), 2);
                bnd_uv_stacked.bottomRows(bnd_uv.rows()) = bnd_uv;
            }

            // Harmonic map with uniform weights
            Eigen::SparseMatrix<double> A, M;
            OptCuts::IglUtils::computeUniformLaplacian(temp->F, A);
            igl::harmonic(A, M, bnd_stacked, bnd_uv_stacked, 1, temp->V);

            if (!temp->checkInversion())
            {
                std::cout << "local injectivity still violated in the computed initial UV map, "
                          << "please carefully check UV topology for e.g. non-manifold vertices. "
                          << "Exit program..." << std::endl;
                exit(-1);
            }
        }

        // 输入
        triSoup.emplace_back(temp);
        outputFolderPath +=
            meshName + "_input_" + OptCuts::IglUtils::rtos(lambda_init) + "_" + OptCuts::IglUtils::rtos(testID) + "_" + startDS + folderTail;
    }
    else
    {
        // no UV provided, compute initial UV

        Eigen::VectorXi C;
        igl::facet_components(F, C);
        int n_components = C.maxCoeff() + 1;
        std::cout << n_components << " disconnected components in total" << std::endl;

        // 构造输入 Mesh|没有UV
        // in each pass, make one cut on each component if needed, until all becoming disk-topology
        OptCuts::TriMesh temp(V, F, Eigen::MatrixXd(), Eigen::MatrixXi(), false);
        std::vector<Eigen::MatrixXi> F_component(n_components);
        std::vector<std::set<int>> V_ind_component(n_components);
        for (int triI = 0; triI < temp.F.rows(); ++triI)
        {
            F_component[C[triI]].conservativeResize(F_component[C[triI]].rows() + 1, 3);
            F_component[C[triI]].bottomRows(1) = temp.F.row(triI);
            for (int i = 0; i < 3; ++i)
            {
                V_ind_component[C[triI]].insert(temp.F(triI, i));
            }
        }
        while (true)
        {
            std::vector<int> components_to_cut;
            for (int componentI = 0; componentI < n_components; ++componentI)
            {
                std::cout << ">>> component " << componentI << std::endl;

                int EC = igl::euler_characteristic(temp.V, F_component[componentI]) - temp.V.rows() + V_ind_component[componentI].size();
                std::cout << "euler_characteristic " << EC << std::endl;
                if (EC < 1)
                {
                    // treat as higher-genus surfaces using cut_to_disk()
                    components_to_cut.emplace_back(-componentI - 1);
                }
                else if (EC == 2)
                {
                    // closed genus-0 surface
                    components_to_cut.emplace_back(componentI);
                }
                else if (EC != 1)
                {
                    std::cout << "unsupported single-connected component!" << std::endl;
                    exit(-1);
                }
            }
            std::cout << components_to_cut.size() << " components to cut to disk" << std::endl;

            if (components_to_cut.empty())
            {
                break;
            }

            for (auto componentI : components_to_cut)
            {
                if (componentI < 0)
                {
                    // cut high genus
                    componentI = -componentI - 1;

                    std::vector<std::vector<int>> cuts;
                    igl::cut_to_disk(F_component[componentI], cuts);  // Meshes with boundary are supported; boundary edges will be included as cuts.
                    std::cout << cuts.size() << " seams to cut component " << componentI << std::endl;

                    // only cut one seam each time to avoid seam vertex id inconsistency
                    int cuts_made = 0;
                    for (auto& seamI : cuts)
                    {
                        if (seamI.front() == seamI.back())
                        {
                            // cutPath() dos not support closed-loop cuts, split it into two cuts
                            cuts_made +=
                                temp.cutPath(std::vector<int>({seamI[seamI.size() - 3], seamI[seamI.size() - 2], seamI[seamI.size() - 1]}), true);
                            temp.initSeams = temp.cohE;
                            seamI.resize(seamI.size() - 2);
                        }
                        cuts_made += temp.cutPath(seamI, true);
                        temp.initSeams = temp.cohE;
                        if (cuts_made)
                        {
                            break;
                        }
                    }

                    if (!cuts_made)
                    {
                        std::cout << "FATAL ERROR: no cuts made when cutting input geometry to disk-topology!" << std::endl;
                        exit(-1);
                    }
                }
                else
                {
                    // cut the topological sphere into a topological disk
                    switch (initCutOption)
                    {
                        case 0:
                            temp.onePointCut(F_component[componentI](0, 0));
                            rand1PInitCut = (n_components == 1);
                            break;

                        case 1:
                            temp.farthestPointCut(F_component[componentI](0, 0));
                            break;

                        default:
                            std::cout << "invalid initCutOption " << initCutOption << std::endl;
                            assert(0);
                            break;
                    }
                }
            }

            // data update on each component for identifying a new cut
            F_component.resize(0);
            F_component.resize(n_components);
            V_ind_component.resize(0);
            V_ind_component.resize(n_components);
            for (int triI = 0; triI < temp.F.rows(); ++triI)
            {
                F_component[C[triI]].conservativeResize(F_component[C[triI]].rows() + 1, 3);
                F_component[C[triI]].bottomRows(1) = temp.F.row(triI);
                for (int i = 0; i < 3; ++i)
                {
                    V_ind_component[C[triI]].insert(temp.F(triI, i));
                }
            }
        }

        int UVGridDim = 0;
        do
        {
            ++UVGridDim;
        } while (UVGridDim * UVGridDim < n_components);
        std::cout << "UVGridDim " << UVGridDim << std::endl;

        // compute boundary UV coordinates, using a grid layout for muliComp
        Eigen::VectorXi bnd_stacked;
        Eigen::MatrixXd bnd_uv_stacked;
        for (int componentI = 0; componentI < n_components; ++componentI)
        {
            std::cout << ">>> component " << componentI << std::endl;

            std::vector<std::vector<int>> bnd_all;
            igl::boundary_loop(F_component[componentI], bnd_all);
            std::cout << "boundary loop count " << bnd_all.size() << std::endl;  // must be 1 for the current initial cut strategy

            int longest_bnd_id = 0;
            for (int bnd_id = 1; bnd_id < bnd_all.size(); ++bnd_id)
            {
                if (bnd_all[longest_bnd_id].size() < bnd_all[bnd_id].size())
                {
                    longest_bnd_id = bnd_id;
                }
            }
            std::cout << "longest_bnd_id " << longest_bnd_id << std::endl;

            bnd_stacked.conservativeResize(bnd_stacked.size() + bnd_all[longest_bnd_id].size());
            bnd_stacked.tail(bnd_all[longest_bnd_id].size()) = Eigen::VectorXi::Map(bnd_all[longest_bnd_id].data(), bnd_all[longest_bnd_id].size());

            Eigen::MatrixXd bnd_uv;
            igl::map_vertices_to_circle(temp.V_rest, bnd_stacked.tail(bnd_all[longest_bnd_id].size()), bnd_uv);
            double xOffset = componentI % UVGridDim * 2.1, yOffset = componentI / UVGridDim * 2.1;
            for (int bnd_uvI = 0; bnd_uvI < bnd_uv.rows(); bnd_uvI++)
            {
                bnd_uv(bnd_uvI, 0) += xOffset;
                bnd_uv(bnd_uvI, 1) += yOffset;
            }
            bnd_uv_stacked.conservativeResize(bnd_uv_stacked.rows() + bnd_uv.rows(), 2);
            bnd_uv_stacked.bottomRows(bnd_uv.rows()) = bnd_uv;
        }

        // Harmonic map with uniform weights
        Eigen::MatrixXd UV_Tutte;
        Eigen::SparseMatrix<double> A, M;
        OptCuts::IglUtils::computeUniformLaplacian(temp.F, A);
        igl::harmonic(A, M, bnd_stacked, bnd_uv_stacked, 1, UV_Tutte);

        // 输入
        triSoup.emplace_back(new OptCuts::TriMesh(V, F, UV_Tutte, temp.F, false));
        outputFolderPath +=
            meshName + "_Tutte_" + OptCuts::IglUtils::rtos(lambda_init) + "_" + OptCuts::IglUtils::rtos(testID) + "_" + startDS + folderTail;
    }

    // initialize UV
    //////////////////////////////////

    mkdir(outputFolderPath.c_str(), 0777);
    outputFolderPath += '/';

    // 保存输入初始状态
    igl::writeOBJ(outputFolderPath + "initial_cuts.obj", triSoup.back()->V_rest, triSoup.back()->F);
    logFile.open(outputFolderPath + "log.txt");
    if (!logFile.is_open())
    {
        std::cout << "failed to create log file, please ensure output directory is created successfully!" << std::endl;
        return -1;
    }

    // setup timer
    timer.new_activity("topology");
    timer.new_activity("descent");
    timer.new_activity("scaffolding");
    timer.new_activity("energyUpdate");

    timer_step.new_activity("matrixComputation");
    timer_step.new_activity("matrixAssembly");
    timer_step.new_activity("symbolicFactorization");
    timer_step.new_activity("numericalFactorization");
    timer_step.new_activity("backSolve");
    timer_step.new_activity("lineSearch");
    timer_step.new_activity("boundarySplit");
    timer_step.new_activity("interiorSplit");
    timer_step.new_activity("cornerMerge");

    // * Our approach
    texScale = 10.0 / (triSoup[0]->bbox.row(1) - triSoup[0]->bbox.row(0)).maxCoeff();
    energyParams.emplace_back(1.0 - lambda_init);
    energyTerms.emplace_back(new OptCuts::SymDirichletEnergy());

    // 优化器
    optimizer = new OptCuts::Optimizer(*triSoup[0], energyTerms, energyParams, 0, false,
                                       bijectiveParam && !rand1PInitCut);  // for random one point initial cut, don't need air meshes in the beginning
                                                                           // since it's impossible for a quad to intersect itself
    optimizer->precompute();

    triSoup.emplace_back(&optimizer->getResult());
    triSoup_backup = optimizer->getResult();
    triSoup.emplace_back(&optimizer->getData_findExtrema());  // for visualizing UV map for finding extrema

    if (lambda_init > 0.0)
    {
        // fracture mode
        fractureMode = true;
    }

    /////////////////////////////////////////////////////////////////////////////
    // regional seam placement
    std::ifstream vWFile(meshFolderPath + "/" + meshName + "_selected.txt");
    if (vWFile.is_open())
    {
        while (!vWFile.eof())
        {
            int selected;
            vWFile >> selected;
            if (selected < optimizer->getResult().vertWeight.size())
            {
                optimizer->getResult().vertWeight[selected] = 100.0;
            }
        }
        vWFile.close();

        OptCuts::IglUtils::smoothVertField(optimizer->getResult(), optimizer->getResult().vertWeight);

        std::cout << "OptCuts with regional seam placement" << std::endl;
    }
    //////////////////////////////////////////////////////////////////////////////

    if (headlessMode)
    {
        // 正式求解计算
        while (true)
        {
            // 退出在哪里??
            preDrawFunc();
            postDrawFunc();
        }
    }

    // 并没有释放
    // Before exit
    logFile.close();
    for (auto& eI : energyTerms)
    {
        delete eI;
    }
    delete optimizer;
    delete triSoup[0];
}