//===-- BuildFile.cpp -----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/BuildSystem/BuildFile.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/YAMLParser.h"

using namespace llbuild;
using namespace llbuild::buildsystem;

BuildFileDelegate::~BuildFileDelegate() {}

Node::~Node() {}

Task::~Task() {}

Tool::~Tool() {}

#pragma mark - BuildFile implementation

namespace {

#ifndef NDEBUG
static void dumpNode(llvm::yaml::Node* node, unsigned indent=0)
    __attribute__((used));
static void dumpNode(llvm::yaml::Node* node, unsigned indent) {
  switch (node->getType()) {
  default: {
    fprintf(stderr, "%*s<node: %p, unknown>\n", indent*2, "", node);
    break;
  }

  case llvm::yaml::Node::NK_Null: {
    fprintf(stderr, "%*s(null)\n", indent*2, "");
    break;
  }

  case llvm::yaml::Node::NK_Scalar: {
    llvm::yaml::ScalarNode* scalar = llvm::cast<llvm::yaml::ScalarNode>(node);
    llvm::SmallString<256> storage;
    fprintf(stderr, "%*s(scalar: '%s')\n", indent*2, "",
            scalar->getValue(storage).str().c_str());
    break;
  }

  case llvm::yaml::Node::NK_KeyValue: {
    assert(0 && "unexpected keyvalue node");
    break;
  }

  case llvm::yaml::Node::NK_Mapping: {
    llvm::yaml::MappingNode* map = llvm::cast<llvm::yaml::MappingNode>(node);
    fprintf(stderr, "%*smap:\n", indent*2, "");
    for (auto& it: *map) {
      fprintf(stderr, "%*skey:\n", (indent+1)*2, "");
      dumpNode(it.getKey(), indent+2);
      fprintf(stderr, "%*svalue:\n", (indent+1)*2, "");
      dumpNode(it.getValue(), indent+2);
    }
    break;
  }

  case llvm::yaml::Node::NK_Sequence: {
    llvm::yaml::SequenceNode* sequence =
      llvm::cast<llvm::yaml::SequenceNode>(node);
    fprintf(stderr, "%*ssequence:\n", indent*2, "");
    for (auto& it: *sequence) {
      dumpNode(&it, indent+1);
    }
    break;
  }

  case llvm::yaml::Node::NK_Alias: {
    fprintf(stderr, "%*s(alias)\n", indent*2, "");
    break;
  }
  }
}
#endif

class BuildFileImpl {
  /// The name of the main input file.
  std::string mainFilename;

  /// The delegate the BuildFile was configured with.
  BuildFileDelegate& delegate;

  /// The set of all registered tools.
  BuildFile::tool_set tools;

  /// The set of all declared targets.
  BuildFile::target_set targets;

  /// The set of all declared nodes.
  BuildFile::node_set nodes;

  /// The set of all declared tasks.
  BuildFile::task_set tasks;
  
  // FIXME: Factor out into a parser helper class.
  std::string stringFromScalarNode(llvm::yaml::ScalarNode* scalar) {
    llvm::SmallString<256> storage;
    return scalar->getValue(storage).str();
  }

  // FIXME: Factor out into a parser helper class.
  bool nodeIsScalarString(llvm::yaml::Node* node, const std::string& name) {
    if (node->getType() != llvm::yaml::Node::NK_Scalar)
      return false;

    return stringFromScalarNode(
        static_cast<llvm::yaml::ScalarNode*>(node)) == name;
  }

  Tool* getOrCreateTool(const std::string& name) {
    // First, check the map.
    auto it = tools.find(name);
    if (it != tools.end())
      return it->second.get();
    
    // Otherwise, ask the delegate to create the tool.
    auto tool = delegate.lookupTool(name);
    if (!tool) {
      delegate.error(mainFilename, "invalid tool type in 'tools' map");
      return nullptr;
    }
    auto result = tool.get();
    tools[name] = std::move(tool);

    return result;
  }

  Node* getOrCreateNode(const std::string& name, bool isImplicit) {
    // First, check the map.
    auto it = nodes.find(name);
    if (it != nodes.end())
      return it->second.get();
    
    // Otherwise, ask the delegate to create the node.
    auto node = delegate.lookupNode(name, isImplicit);
    assert(node);
    auto result = node.get();
    nodes[name] = std::move(node);

    return result;
  }

  bool parseRootNode(llvm::yaml::Node* node) {
    // The root must always be a mapping.
    if (node->getType() != llvm::yaml::Node::NK_Mapping) {
      delegate.error(mainFilename, "unexpected top-level node");
      return false;
    }
    auto mapping = static_cast<llvm::yaml::MappingNode*>(node);

    // Iterate over each of the sections in the mapping.
    auto it = mapping->begin();
    if (!nodeIsScalarString(it->getKey(), "client")) {
      delegate.error(mainFilename, "expected initial mapping key 'client'");
      return false;
    }
    if (it->getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
      delegate.error(mainFilename, "unexpected 'client' value (expected map)");
      return false;
    }

    // Parse the client mapping.
    if (!parseClientMapping(
            static_cast<llvm::yaml::MappingNode*>(it->getValue()))) {
      return false;
    }
    ++it;

    // Parse the tools mapping, if present.
    if (it != mapping->end() && nodeIsScalarString(it->getKey(), "tools")) {
      if (it->getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        delegate.error(mainFilename, "unexpected 'tools' value (expected map)");
        return false;
      }

      if (!parseToolsMapping(
              static_cast<llvm::yaml::MappingNode*>(it->getValue()))) {
        return false;
      }
      ++it;
    }

    // Parse the targets mapping, if present.
    if (it != mapping->end() && nodeIsScalarString(it->getKey(), "targets")) {
      if (it->getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        delegate.error(
            mainFilename, "unexpected 'targets' value (expected map)");
        return false;
      }

      if (!parseTargetsMapping(
              static_cast<llvm::yaml::MappingNode*>(it->getValue()))) {
        return false;
      }
      ++it;
    }

    // Parse the nodes mapping, if present.
    if (it != mapping->end() && nodeIsScalarString(it->getKey(), "nodes")) {
      if (it->getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        delegate.error(
            mainFilename, "unexpected 'nodes' value (expected map)");
        return false;
      }

      if (!parseNodesMapping(
              static_cast<llvm::yaml::MappingNode*>(it->getValue()))) {
        return false;
      }
      ++it;
    }

    // Parse the tasks mapping, if present.
    if (it != mapping->end() && nodeIsScalarString(it->getKey(), "tasks")) {
      if (it->getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        delegate.error(
            mainFilename, "unexpected 'tasks' value (expected map)");
        return false;
      }

      if (!parseTasksMapping(
              static_cast<llvm::yaml::MappingNode*>(it->getValue()))) {
        return false;
      }
      ++it;
    }

    // There shouldn't be any trailing sections.
    if (it != mapping->end()) {
      delegate.error(mainFilename, "unexpected trailing top-level section");
      return false;
    }

    return true;
  }

  bool parseClientMapping(llvm::yaml::MappingNode* map) {
    // Collect all of the keys.
    std::string name;
    uint32_t version = 0;
    property_list_type properties;

    for (auto& entry: *map) {
      // All keys and values must be scalar.
      if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
        delegate.error(mainFilename, "invalid key type in 'client' map");
        return false;
      }
      if (entry.getValue()->getType() != llvm::yaml::Node::NK_Scalar) {
        delegate.error(mainFilename, "invalid value type in 'client' map");
        return false;
      }

      std::string key = stringFromScalarNode(
          static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
      std::string value = stringFromScalarNode(
          static_cast<llvm::yaml::ScalarNode*>(entry.getValue()));
      if (key == "name") {
        name = value;
      } else if (key == "version") {
        if (llvm::StringRef(value).getAsInteger(10, version)) {
          delegate.error(
              mainFilename, "invalid version number in 'client' map");
          return false;
        }
      } else {
        properties.push_back({ key, value });
      }
    }

    // Pass to the delegate.
    if (!delegate.configureClient(name, version, properties)) {
      delegate.error(mainFilename, "unable to configure client");
      return false;
    }

    return true;
  }

  bool parseToolsMapping(llvm::yaml::MappingNode* map) {
    for (auto& entry: *map) {
      // Every key must be scalar.
      if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
        delegate.error(mainFilename, "invalid key type in 'tools' map");
        return false;
      }
      // Every value must be a mapping.
      if (entry.getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        delegate.error(mainFilename, "invalid value type in 'tools' map");
        return false;
      }

      std::string name = stringFromScalarNode(
          static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
      llvm::yaml::MappingNode* attrs = static_cast<llvm::yaml::MappingNode*>(
          entry.getValue());

      // Get the tool.
      auto tool = getOrCreateTool(name);
      if (!tool) {
        return false;
      }

      // Configure all of the tool attributes.
      for (auto& valueEntry: *attrs) {
        auto key = valueEntry.getKey();
        auto value = valueEntry.getValue();
        
        // All keys and values must be scalar.
        if (key->getType() != llvm::yaml::Node::NK_Scalar) {
          delegate.error(mainFilename, "invalid key type in 'tools' map");
          return false;
        }
        if (value->getType() != llvm::yaml::Node::NK_Scalar) {
          delegate.error(mainFilename, "invalid value type in 'tools' map");
          return false;
        }

        if (!tool->configureAttribute(
                stringFromScalarNode(
                    static_cast<llvm::yaml::ScalarNode*>(key)),
                stringFromScalarNode(
                    static_cast<llvm::yaml::ScalarNode*>(value)))) {
          return false;
        }
      }
    }

    return true;
  }
  
  bool parseTargetsMapping(llvm::yaml::MappingNode* map) {
    for (auto& entry: *map) {
      // Every key must be scalar.
      if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
        delegate.error(mainFilename, "invalid key type in 'targets' map");
        return false;
      }
      // Every value must be a sequence.
      if (entry.getValue()->getType() != llvm::yaml::Node::NK_Sequence) {
        delegate.error(mainFilename, "invalid value type in 'targets' map");
        return false;
      }

      std::string name = stringFromScalarNode(
          static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
      llvm::yaml::SequenceNode* nodes = static_cast<llvm::yaml::SequenceNode*>(
          entry.getValue());

      // Create the target.
      std::unique_ptr<Target> target(new Target(name));

      // Add all of the nodes.
      for (auto& node: *nodes) {
        // All items must be scalar.
        if (node.getType() != llvm::yaml::Node::NK_Scalar) {
          delegate.error(mainFilename, "invalid node type in 'targets' map");
          return false;
        }

        target->getNodeNames().push_back(
            stringFromScalarNode(
                static_cast<llvm::yaml::ScalarNode*>(&node)));
      }

      // Let the delegate know we loaded a target.
      delegate.loadedTarget(name, *target);

      // Add the target to the targets map.
      targets[name] = std::move(target);
    }

    return true;
  }

  bool parseNodesMapping(llvm::yaml::MappingNode* map) {
    for (auto& entry: *map) {
      // Every key must be scalar.
      if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
        delegate.error(mainFilename, "invalid key type in 'nodes' map");
        return false;
      }
      // Every value must be a mapping.
      if (entry.getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        delegate.error(mainFilename, "invalid value type in 'nodes' map");
        return false;
      }

      std::string name = stringFromScalarNode(
          static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
      llvm::yaml::MappingNode* attrs = static_cast<llvm::yaml::MappingNode*>(
          entry.getValue());

      // Get the node.
      //
      // FIXME: One downside of doing the lookup here is that the client cannot
      // ever make a context dependent node that can have configured properties.
      auto node = getOrCreateNode(name, /*isImplicit=*/false);

      // Configure all of the tool attributes.
      for (auto& valueEntry: *attrs) {
        auto key = valueEntry.getKey();
        auto value = valueEntry.getValue();
        
        // All keys and values must be scalar.
        if (key->getType() != llvm::yaml::Node::NK_Scalar) {
          delegate.error(mainFilename, "invalid key type in 'tools' map");
          return false;
        }
        if (value->getType() != llvm::yaml::Node::NK_Scalar) {
          delegate.error(mainFilename, "invalid value type in 'tools' map");
          return false;
        }

        if (!node->configureAttribute(
                stringFromScalarNode(
                    static_cast<llvm::yaml::ScalarNode*>(key)),
                stringFromScalarNode(
                    static_cast<llvm::yaml::ScalarNode*>(value)))) {
          return false;
        }
      }
    }

    return true;
  }

  bool parseTasksMapping(llvm::yaml::MappingNode* map) {
    for (auto& entry: *map) {
      // Every key must be scalar.
      if (entry.getKey()->getType() != llvm::yaml::Node::NK_Scalar) {
        delegate.error(mainFilename, "invalid key type in 'tasks' map");
        return false;
      }
      // Every value must be a mapping.
      if (entry.getValue()->getType() != llvm::yaml::Node::NK_Mapping) {
        delegate.error(mainFilename, "invalid value type in 'tasks' map");
        return false;
      }

      std::string name = stringFromScalarNode(
          static_cast<llvm::yaml::ScalarNode*>(entry.getKey()));
      llvm::yaml::MappingNode* attrs = static_cast<llvm::yaml::MappingNode*>(
          entry.getValue());
      
      // Get the initial attribute, which must be the tool name.
      auto it = attrs->begin();
      if (it == attrs->end()) {
        delegate.error(mainFilename, "missing 'tool' key in 'task' map");
        return false;
      }
      if (!nodeIsScalarString(it->getKey(), "tool")) {
        delegate.error(
            mainFilename, "expected 'tool' initial key in 'tasks' map");
        return false;
      }
      if (it->getValue()->getType() != llvm::yaml::Node::NK_Scalar) {
        delegate.error(
            mainFilename, "invalid 'tool' value type in 'tasks' map");
        return false;
      }
      
      // Lookup the tool for this task.
      auto tool = getOrCreateTool(
          stringFromScalarNode(
              static_cast<llvm::yaml::ScalarNode*>(
                  it->getValue())));
      if (!tool) {
        return false;
      }
        
      // Create the task.
      auto task = tool->createTask(name);

      // Parse the remaining task attributes.
      ++it;
      for (; it != attrs->end(); ++it) {
        auto key = it->getKey();
        auto value = it->getValue();
        
        // If this is a known key, parse it.
        if (nodeIsScalarString(key, "inputs")) {
          if (value->getType() != llvm::yaml::Node::NK_Sequence) {
            delegate.error(
                mainFilename, "invalid value type for 'inputs' task key");
            return false;
          }

          llvm::yaml::SequenceNode* nodeNames =
            static_cast<llvm::yaml::SequenceNode*>(value);

          std::vector<Node*> nodes;
          for (auto& nodeName: *nodeNames) {
            if (nodeName.getType() != llvm::yaml::Node::NK_Scalar) {
              delegate.error(
                  mainFilename, "invalid node type in 'inputs' task key");
              return false;
            }

            nodes.push_back(
                getOrCreateNode(
                    stringFromScalarNode(
                        static_cast<llvm::yaml::ScalarNode*>(&nodeName)),
                    /*isImplicit=*/true));
          }

          task->configureInputs(nodes);
        } else if (nodeIsScalarString(key, "outputs")) {
          if (value->getType() != llvm::yaml::Node::NK_Sequence) {
            delegate.error(
                mainFilename, "invalid value type for 'outputs' task key");
            return false;
          }

          llvm::yaml::SequenceNode* nodeNames =
            static_cast<llvm::yaml::SequenceNode*>(value);

          std::vector<Node*> nodes;
          for (auto& nodeName: *nodeNames) {
            if (nodeName.getType() != llvm::yaml::Node::NK_Scalar) {
              delegate.error(
                  mainFilename, "invalid node type in 'outputs' task key");
              return false;
            }

            nodes.push_back(
                getOrCreateNode(
                    stringFromScalarNode(
                        static_cast<llvm::yaml::ScalarNode*>(&nodeName)),
                    /*isImplicit=*/true));
          }

          task->configureOutputs(nodes);
        } else {
          // Otherwise, it should be an attribute string key value pair.
          
          // All keys and values must be scalar.
          if (key->getType() != llvm::yaml::Node::NK_Scalar) {
            delegate.error(mainFilename, "invalid key type in 'tools' map");
            return false;
          }
          if (value->getType() != llvm::yaml::Node::NK_Scalar) {
            delegate.error(mainFilename, "invalid value type in 'tools' map");
            return false;
          }

          if (!task->configureAttribute(
                  stringFromScalarNode(
                      static_cast<llvm::yaml::ScalarNode*>(key)),
                  stringFromScalarNode(
                      static_cast<llvm::yaml::ScalarNode*>(value)))) {
            return false;
          }
        }
      }

      // Let the delegate know we loaded a task.
      delegate.loadedTask(name, *task);

      // Add the task to the tasks map.
      tasks[name] = std::move(task);
    }

    return true;
  }

public:
  BuildFileImpl(class BuildFile& buildFile,
                const std::string& mainFilename,
                BuildFileDelegate& delegate)
    : mainFilename(mainFilename), delegate(delegate) {}

  BuildFileDelegate* getDelegate() {
    return &delegate;
  }

  /// @name Parse Actions
  /// @{

  bool load() {
    // Create a memory buffer for the input.
    //
    // FIXME: Lift the file access into the delegate, like we do for Ninja.
    llvm::SourceMgr sourceMgr;
    auto res = llvm::MemoryBuffer::getFile(
        mainFilename);
    if (auto ec = res.getError()) {
      delegate.error(mainFilename,
                     ("unable to open '" + mainFilename +
                      "' (" + ec.message() + ")"));
      return false;
    }

    std::unique_ptr<llvm::MemoryBuffer> input(res->release());

    // Create a YAML parser.
    llvm::yaml::Stream stream(input->getMemBufferRef(), sourceMgr);

    // Read the stream, we only expect a single document.
    auto it = stream.begin();
    if (it == stream.end()) {
      delegate.error(mainFilename, "missing document in stream");
      return false;
    }

    auto& document = *it;
    if (!parseRootNode(document.getRoot())) {
      return false;
    }

    if (++it != stream.end()) {
      delegate.error(mainFilename, "unexpected additional document in stream");
      return false;
    }

    return true;
  }

  /// @name Accessors
  /// @{

  const BuildFile::node_set& getNodes() const { return nodes; }

  const BuildFile::target_set& getTargets() const { return targets; }

  const BuildFile::task_set& getTasks() const { return tasks; }

  const BuildFile::tool_set& getTools() const { return tools; }

  /// @}
};

}

#pragma mark - BuildFile

BuildFile::BuildFile(const std::string& mainFilename,
                     BuildFileDelegate& delegate)
  : impl(new BuildFileImpl(*this, mainFilename, delegate))
{
}

BuildFile::~BuildFile() {
  delete static_cast<BuildFileImpl*>(impl);
}

BuildFileDelegate* BuildFile::getDelegate() {
  return static_cast<BuildFileImpl*>(impl)->getDelegate();
}

const BuildFile::node_set& BuildFile::getNodes() const {
  return static_cast<BuildFileImpl*>(impl)->getNodes();
}

const BuildFile::target_set& BuildFile::getTargets() const {
  return static_cast<BuildFileImpl*>(impl)->getTargets();
}

const BuildFile::task_set& BuildFile::getTasks() const {
  return static_cast<BuildFileImpl*>(impl)->getTasks();
}

const BuildFile::tool_set& BuildFile::getTools() const {
  return static_cast<BuildFileImpl*>(impl)->getTools();
}

bool BuildFile::load() {
  return static_cast<BuildFileImpl*>(impl)->load();
}