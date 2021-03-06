//
// Copyright © 2018-2021 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#include "Cascading.hpp"

#include "../Graph.hpp"
#include "../GraphNodes.hpp"
#include "../Utils.hpp"
#include "DebuggingContext.hpp"
#include "Estimation.hpp"
#include "EstimationUtils.hpp"
#include "Part.hpp"

#include "../include/ethosn_support_library/Optional.hpp"
#include <ethosn_utils/Filesystem.hpp>

#include <fstream>
#include <iostream>

using namespace std;
using namespace ethosn::utils;

namespace ethosn
{
namespace support_library
{

namespace
{

template <typename T>
bool IsNodeOfType(const Node* node)
{
    return (dynamic_cast<const T*>(node) != nullptr);
}

void SaveDebugFilesForUnestimatedCombination(std::string folder,
                                             const DebuggingContext& debuggingContext,
                                             const Combination& comb,
                                             const OpGraph& opGraph,
                                             const GraphOfParts& graphOfParts)
{
    MakeDirectory(debuggingContext.GetAbsolutePathOutputFileName(folder).c_str());

    debuggingContext.SaveCombinationToDot(CompilationOptions::DebugLevel::None, comb, graphOfParts,
                                          folder + "/Simple.dot", DetailLevel::Low);
    debuggingContext.SaveCombinationToDot(CompilationOptions::DebugLevel::None, comb, graphOfParts,
                                          folder + "/Detailed.dot", DetailLevel::High);

    debuggingContext.SaveOpGraphToDot(CompilationOptions::DebugLevel::None, opGraph, folder + "/MergedSimple.dot",
                                      DetailLevel::Low);
    debuggingContext.SaveOpGraphToDot(CompilationOptions::DebugLevel::None, opGraph, folder + "/MergedDetailed.dot",
                                      DetailLevel::High);
}

void SaveDebugFilesForEstimatedCombination(std::string folder,
                                           const DebuggingContext& debuggingContext,
                                           const OpGraph& opGraph,
                                           const EstimatedOpGraph& estimationDetails)
{
    MakeDirectory(debuggingContext.GetAbsolutePathOutputFileName(folder).c_str());

    debuggingContext.SaveEstimatedOpGraphToDot(CompilationOptions::DebugLevel::None, opGraph, estimationDetails,
                                               folder + "/Estimated.dot", DetailLevel::High);
}

}    // namespace

GraphOfParts CreateGraphOfParts(const Graph& graph,
                                const EstimationOptions& estOpt,
                                const CompilationOptions& compOpt,
                                const HardwareCapabilities& capabilities)
{
    GraphOfParts graphOfParts;
    Parts& parts = graphOfParts.m_Parts;

    auto AddNodeToPart    = [](Node* node, Part& part) -> void { part.m_SubGraph.push_back(node); };
    auto AddNodeToNewPart = [&](Node* node) -> void {
        // Insert node into new part.
        parts.push_back(std::make_unique<Part>(estOpt, compOpt, capabilities));
        AddNodeToPart(node, *(parts.back()));
    };
    auto FindPartFromSourceAndAddNode = [&](Node* ppOpNode) -> void {
        // Iterate in reverse, it will be quicker.
        for (auto part = parts.rbegin(); part != parts.rend(); ++part)
        {
            // Connect PP Op nodes only if the parent node has a single output.
            const auto partOutputNode = (*part)->m_SubGraph.back();
            for (const auto input : ppOpNode->GetInputs())
            {
                if (input->GetSource() == partOutputNode)
                {
                    // Case 1)
                    AddNodeToPart(ppOpNode, **part);
                    return;
                }
            }
        }
        assert(!"MCE Post-Process node has not been added to any Part");
    };

    for (Node* node : graph.GetNodesSorted())
    {
        assert(node);
        if (IsNodeOfType<McePostProcessOperationNode>(node))
        {
            // There are two possible cases with PP Op nodes:
            // 1) The node is connected to an MCE operation node with a single output.
            // 2) The node is connected to a non PP Op node with multiple outputs.
            // If 1), then find the part with the source node and add this node.
            // If 2), then create a new part with that single PP Op node.

            auto source = node->GetInputs()[0]->GetSource();
            if (IsNodeOfType<MceOperationNode>(source) && source->GetOutputs().size() == 1)
            {
                // Case 1)
                FindPartFromSourceAndAddNode(node);
            }
            else
            {
                // Case 2)
                AddNodeToNewPart(node);
            }
        }
        else
        {
            AddNodeToNewPart(node);
        }
    }

    // Validate that every node has been assigned to a Part.
    std::set<Node*> nodes;
    std::transform(graph.GetNodes().begin(), graph.GetNodes().end(), std::inserter(nodes, nodes.end()),
                   [](auto&& n) { return n.get(); });
    for (auto&& p : graphOfParts.m_Parts)
    {
        for (auto&& n : p->m_SubGraph)
        {
            nodes.erase(n);
        }
    }
    if (!nodes.empty())
    {
        throw NotSupportedException("Some nodes could not be assigned to a Part");
    }

    return graphOfParts;
}

void CreatePlans(Parts& parts)
{
    for (auto& part : parts)
    {
        part->CreatePlans();
    }

    return;
}

Cascading::Cascading(const EstimationOptions& estOpt,
                     const CompilationOptions& compOpt,
                     const HardwareCapabilities& hwCap)
    : IEstimationStrategy(estOpt, compOpt, hwCap)
    , m_BestCombination(nullptr)
{
    // Constructor
}

Cascading::~Cascading()
{}

NetworkPerformanceData Cascading::Estimate(Graph& graph)
{
    m_GraphOfParts = CreateGraphOfParts(graph, m_EstimationOptions, m_CompilationOptions, m_Capabilities);

    m_DebuggingContext.SaveGraphToDot(CompilationOptions::DebugLevel::Medium, graph, &m_GraphOfParts,
                                      "Cascaded_GraphOfParts.dot", DetailLevel::Low);
    m_DebuggingContext.SaveGraphToDot(CompilationOptions::DebugLevel::Medium, graph, &m_GraphOfParts,
                                      "Cascaded_GraphOfPartsDetailed.dot", DetailLevel::High);

    CreatePlans(m_GraphOfParts.m_Parts);

    if (m_DebuggingContext.m_DebugInfo->m_DumpDebugFiles >= CompilationOptions::DebugLevel::Medium)
    {
        std::ofstream debugPlanCountsDumpFile(
            m_DebuggingContext.GetAbsolutePathOutputFileName("Cascaded_PlanCounts.txt"));

        MakeDirectory(m_DebuggingContext.GetAbsolutePathOutputFileName("Parts").c_str());

        for (auto&& part : m_GraphOfParts.m_Parts)
        {
            std::string folder = "Parts/" + part->m_DebugTag;
            MakeDirectory(m_DebuggingContext.GetAbsolutePathOutputFileName(folder).c_str());

            debugPlanCountsDumpFile << part->m_DebugTag << ": " << part->GetNumPlans() << std::endl;

            m_DebuggingContext.SavePlansToDot(CompilationOptions::DebugLevel::Medium, *part, folder + "/Plans.dot",
                                              DetailLevel::Low);
            m_DebuggingContext.SavePlansToDot(CompilationOptions::DebugLevel::Medium, *part,
                                              folder + "/PlansDetailed.dot", DetailLevel::High);
        }
    }

    m_ValidCombinations = Combine(m_GraphOfParts);

    if (m_DebuggingContext.m_DebugInfo->m_DumpDebugFiles >= CompilationOptions::DebugLevel::High)
    {
        MakeDirectory(m_DebuggingContext.GetAbsolutePathOutputFileName("Combinations").c_str());
        uint32_t counter = 0;
        for (const Combination& comb : m_ValidCombinations)
        {
            std::string folder = "Combinations/" + std::to_string(counter);
            OpGraph g          = GetOpGraphForCombination(comb, m_GraphOfParts);
            SaveDebugFilesForUnestimatedCombination(folder, m_DebuggingContext, comb, g, m_GraphOfParts);
            ++counter;
        }
    }

    if (m_ValidCombinations.empty())
    {
        throw NotSupportedException("No valid combinations were found.");
    }

    EstimatePerformance();
    return m_PerformanceStream;
}

const GraphOfParts& Cascading::GetGraphOfParts() const
{
    return m_GraphOfParts;
}

const ethosn::support_library::Combination* Cascading::GetBestCombination()
{
    return m_BestCombination;
}

void Cascading::EstimatePerformance()
{
    std::ofstream debugPerformanceDumpFile;
    if (m_DebuggingContext.m_DebugInfo->m_DumpDebugFiles >= CompilationOptions::DebugLevel::Medium)
    {
        debugPerformanceDumpFile.open(m_DebuggingContext.GetAbsolutePathOutputFileName("Cascaded_Performance.txt"));
    }
    uint32_t combinationIdx = 0;
    utils::Optional<uint32_t> bestCombinationIdx;
    for (const Combination& combination : m_ValidCombinations)
    {
        try
        {
            OpGraph combiOpGraph = GetOpGraphForCombination(combination, m_GraphOfParts);
            EstimatedOpGraph curNetPerfData =
                ethosn::support_library::EstimateOpGraph(combiOpGraph, m_Capabilities, GetEstimationOptions());

            if (m_DebuggingContext.m_DebugInfo->m_DumpDebugFiles >= CompilationOptions::DebugLevel::Medium)
            {
                debugPerformanceDumpFile << combinationIdx << ": "
                                         << GetPerformanceTotalDataMetric(curNetPerfData.m_PerfData) << std::endl;
                if (m_DebuggingContext.m_DebugInfo->m_DumpDebugFiles >= CompilationOptions::DebugLevel::High)
                {
                    std::string folder = "Combinations/" + std::to_string(combinationIdx);
                    SaveDebugFilesForEstimatedCombination(folder, m_DebuggingContext, combiOpGraph, curNetPerfData);
                }
            }

            if (!bestCombinationIdx.has_value() ||
                IsLeftMoreDataPerformantThanRight(curNetPerfData.m_PerfData, m_PerformanceStream))
            {
                m_PerformanceStream = curNetPerfData.m_PerfData;
                m_BestCombination   = &combination;
                bestCombinationIdx  = combinationIdx;
            }
        }
        catch (const NotSupportedException& e)
        {
            // Ignore this combination - others may still be valid
            if (m_DebuggingContext.m_DebugInfo->m_DumpDebugFiles >= CompilationOptions::DebugLevel::Medium)
            {
                debugPerformanceDumpFile << combinationIdx << ": Error: " << e.what() << std::endl;
            }
        }

        ++combinationIdx;
    }

    if (m_DebuggingContext.m_DebugInfo->m_DumpDebugFiles >= CompilationOptions::DebugLevel::Medium)
    {
        debugPerformanceDumpFile << "\nBest: "
                                 << (bestCombinationIdx.has_value() ? std::to_string(bestCombinationIdx.value())
                                                                    : "NONE")
                                 << std::endl;

        // Save the details of the best combination. Note this is done at Medium debug level, so we do this even though
        // we save out details for ALL the combinations on High debug level.
        if (bestCombinationIdx.has_value())
        {
            MakeDirectory(m_DebuggingContext.GetAbsolutePathOutputFileName("Combinations").c_str());
            std::string folder   = "Combinations/Best(" + std::to_string(bestCombinationIdx.value()) + ")";
            OpGraph combiOpGraph = GetOpGraphForCombination(*m_BestCombination, m_GraphOfParts);
            EstimatedOpGraph curNetPerfData =
                ethosn::support_library::EstimateOpGraph(combiOpGraph, m_Capabilities, GetEstimationOptions());
            SaveDebugFilesForUnestimatedCombination(folder, m_DebuggingContext, *m_BestCombination, combiOpGraph,
                                                    m_GraphOfParts);
            SaveDebugFilesForEstimatedCombination(folder, m_DebuggingContext, combiOpGraph, curNetPerfData);
        }
    }
}

}    // namespace support_library
}    // namespace ethosn
