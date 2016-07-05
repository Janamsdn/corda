package com.r3corda.node.visualiser

import com.r3corda.core.contracts.CommandData
import com.r3corda.core.contracts.ContractState
import com.r3corda.core.crypto.SecureHash
import com.r3corda.core.testing.*
import org.graphstream.graph.Edge
import org.graphstream.graph.Node
import org.graphstream.graph.implementations.SingleGraph
import kotlin.reflect.memberProperties

class GraphVisualiser(val dsl: LedgerDSL<LastLineShouldTestForVerifiesOrFails, TestTransactionDSLInterpreter, TestLedgerDSLInterpreter>) {
    companion object {
        val css = GraphVisualiser::class.java.getResourceAsStream("graph.css").bufferedReader().readText()
    }

    fun convert(): SingleGraph {
        val tg = dsl.interpreter.toTransactionGroup()
        val graph = createGraph("Transaction group", css)

        // Map all the transactions, including the bogus non-verified ones (with no inputs) to graph nodes.
        for ((txIndex, tx) in (tg.transactions + tg.nonVerifiedRoots).withIndex()) {
            val txNode = graph.addNode<Node>("tx$txIndex")
            if (tx !in tg.nonVerifiedRoots)
                txNode.label = dsl.interpreter.transactionName(tx.id).let { it ?: "TX[${tx.id.prefixChars()}]" }
            txNode.styleClass = "tx"

            // Now create a vertex for each output state.
            for (outIndex in tx.outputs.indices) {
                val node = graph.addNode<Node>(tx.outRef<ContractState>(outIndex).ref.toString())
                val state = tx.outputs[outIndex]
                node.label = stateToLabel(state.data)
                node.styleClass = stateToCSSClass(state.data) + ",state"
                node.setAttribute("state", state)
                val edge = graph.addEdge<Edge>("tx$txIndex-out$outIndex", txNode, node, true)
                edge.weight = 0.7
            }

            // And a vertex for each command.
            for ((index, cmd) in tx.commands.withIndex()) {
                val node = graph.addNode<Node>(SecureHash.randomSHA256().prefixChars())
                node.label = commandToTypeName(cmd.value)
                node.styleClass = "command"
                val edge = graph.addEdge<Edge>("tx$txIndex-cmd-$index", node, txNode)
                edge.weight = 0.4
            }
        }
        // And now all states and transactions were mapped to graph nodes, hook up the input edges.
        for ((txIndex, tx) in tg.transactions.withIndex()) {
            for ((inputIndex, ref) in tx.inputs.withIndex()) {
                val edge = graph.addEdge<Edge>("tx$txIndex-in$inputIndex", ref.toString(), "tx$txIndex", true)
                edge.weight = 1.2
            }
        }
        return graph
    }

    private fun stateToLabel(state: ContractState): String {
        return dsl.interpreter.outputToLabel(state) ?: stateToTypeName(state)
    }

    private fun commandToTypeName(state: CommandData) = state.javaClass.canonicalName.removePrefix("contracts.").replace('$', '.')
    private fun stateToTypeName(state: ContractState) = state.javaClass.canonicalName.removePrefix("contracts.").removeSuffix(".State")
    private fun stateToCSSClass(state: ContractState) = stateToTypeName(state).replace('.', '_').toLowerCase()

    fun display() {
        runGraph(convert(), nodeOnClick = { node ->
            val state: ContractState? = node.getAttribute("state")
            if (state != null) {
                val props: List<Pair<String, Any?>> = state.javaClass.kotlin.memberProperties.map { it.name to it.getter.call(state) }
                StateViewer.show(props)
            }
        })
    }
}
