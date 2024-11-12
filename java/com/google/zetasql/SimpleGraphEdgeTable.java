/*
 * Copyright 2019 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

package com.google.zetasql;

import static com.google.common.base.Preconditions.checkNotNull;
import static com.google.common.collect.ImmutableSet.toImmutableSet;

import com.google.common.base.Splitter;
import com.google.common.collect.ImmutableList;
import com.google.zetasql.SimplePropertyGraphProtos.SimpleGraphElementTableProto;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** A concrete implementation of the {@link GraphEdgeTable} interface. */
public final class SimpleGraphEdgeTable extends SimpleGraphElementTable implements GraphEdgeTable {
  private final GraphNodeTableReference sourceNode;
  private final GraphNodeTableReference destinationNode;

  public SimpleGraphEdgeTable(
      String name,
      List<String> propertyGraphNamePath,
      Table table,
      List<Integer> keyColumns,
      Set<GraphElementLabel> labels,
      Set<GraphPropertyDefinition> propertyDefinitions,
      GraphNodeTableReference sourceNode,
      GraphNodeTableReference destinationNode) {
    super(name, propertyGraphNamePath, table, keyColumns, labels, propertyDefinitions);
    this.sourceNode = sourceNode;
    this.destinationNode = destinationNode;
  }

  @Override
  public Kind getKind() {
    return Kind.EDGE;
  }

  @Override
  public SimpleGraphElementTableProto serialize(
      FileDescriptorSetsBuilder fileDescriptorSetsBuilder) {
    SimpleGraphElementTableProto.Builder proto =
        super.serialize(fileDescriptorSetsBuilder).toBuilder()
            .setKind(SimpleGraphElementTableProto.Kind.EDGE);

    if (sourceNode instanceof SimpleGraphNodeTableReference) {
      proto.setSourceNodeTable(((SimpleGraphNodeTableReference) sourceNode).serialize());
    } else {
      throw new IllegalArgumentException(
          "Cannot serialize non-SimpleGraphNodeTableReference "
              + sourceNode.getReferencedNodeTable().getName());
    }

    if (destinationNode instanceof SimpleGraphNodeTableReference) {
      proto.setDestNodeTable(((SimpleGraphNodeTableReference) destinationNode).serialize());
    } else {
      throw new IllegalArgumentException(
          "Cannot serialize non-SimpleGraphNodeTableReference "
              + destinationNode.getReferencedNodeTable().getName());
    }
    return proto.build();
  }

  public static SimpleGraphEdgeTable deserialize(
      SimpleGraphElementTableProto proto,
      SimpleCatalog catalog,
      ImmutableList<? extends DescriptorPool> pools,
      Map<String, SimpleGraphNodeTable> nodeTableMap,
      Map<String, SimpleGraphElementLabel> labelMap,
      Map<String, SimpleGraphPropertyDeclaration> propertyDclMap) {
    try {
      return new SimpleGraphEdgeTable(
          proto.getName(),
          proto.getPropertyGraphNamePathList(),
          catalog.findTable(
              ImmutableList.copyOf(Splitter.on('.').split(proto.getInputTableName()))),
          proto.getKeyColumnsList(),
          proto.getLabelNamesList().stream()
              .map(name -> checkNotNull(labelMap.get(name), "label not found: %s", name))
              .collect(toImmutableSet()),
          proto.getPropertyDefinitionsList().stream()
              .map(
                  propertyDefProto ->
                      SimpleGraphPropertyDefinition.deserialize(
                          propertyDefProto, catalog, pools, propertyDclMap))
              .collect(toImmutableSet()),
          SimpleGraphNodeTableReference.deserialize(proto.getSourceNodeTable(), nodeTableMap),
          SimpleGraphNodeTableReference.deserialize(proto.getDestNodeTable(), nodeTableMap));
    } catch (NotFoundException unused) {
      throw new NullPointerException(
          String.format("Could not find table %s in catalog.", proto.getInputTableName()));
    }
  }

  @Override
  public GraphNodeTableReference getSourceNodeTable() {
    return sourceNode;
  }

  @Override
  public GraphNodeTableReference getDestNodeTable() {
    return destinationNode;
  }
}
