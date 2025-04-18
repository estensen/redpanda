
# Image URL to use all building/pushing image targets
OPERATOR_IMG_LATEST ?= "vectorized/redpanda-operator:dev"
CONFIGURATOR_IMG_LATEST ?= "vectorized/configurator:dev"
# Produce CRDs that work back to Kubernetes 1.11 (no version conversion)
CRD_OPTIONS ?= "crd:trivialVersions=true,preserveUnknownFields=false"

# default redpanda image to load
REDPANDA_IMG ?= "localhost/redpanda:dev"

ifeq (aarch64,$(uname -m))
TARGETARCH = arm64
else
TARGETARCH = amd64
endif

SHELL := /bin/bash

# Get the currently used golang install path (in GOPATH/bin, unless GOBIN is set)
ifeq (,$(shell go env GOBIN))
GOBIN=$(shell go env GOPATH)/bin
else
GOBIN=$(shell go env GOBIN)
endif

ifneq ($(TEST_NAME), )
TEST_ONLY_FLAG = --test $(TEST_NAME)
endif

all: manager

# Run tests
ENVTEST_ASSETS_DIR=$(shell pwd)/testbin
test: generate fmt vet manifests
	mkdir -p ${ENVTEST_ASSETS_DIR}
	test -f ${ENVTEST_ASSETS_DIR}/setup-envtest.sh || curl -sSLo ${ENVTEST_ASSETS_DIR}/setup-envtest.sh https://raw.githubusercontent.com/kubernetes-sigs/controller-runtime/v0.7.0/hack/setup-envtest.sh
	source ${ENVTEST_ASSETS_DIR}/setup-envtest.sh; fetch_envtest_tools $(ENVTEST_ASSETS_DIR); setup_envtest_env $(ENVTEST_ASSETS_DIR); go test --race -v ./... -coverprofile cover.out

# Build manager binary
manager: generate fmt vet
	go build -o bin/manager main.go

# Run against the configured Kubernetes cluster in ~/.kube/config
run: generate fmt vet manifests
	go run ./main.go

# Install CRDs into a cluster
install: manifests kustomize
	$(KUSTOMIZE) build config/crd | kubectl apply -f -

# Uninstall CRDs from a cluster
uninstall: manifests kustomize
	$(KUSTOMIZE) build config/crd | kubectl delete -f -

# Deploy controller in the configured Kubernetes cluster in ~/.kube/config
deploy: manifests kustomize
	cd config/manager && $(KUSTOMIZE) edit set image vectorized/redpanda-operator=${OPERATOR_IMG_LATEST}
	$(KUSTOMIZE) build config/default | kubectl apply -f -
	kind load docker-image ${REDPANDA_IMG}

# Deploy pre loaded controller in the configured Kind Kubernetes cluster
deploy-to-kind: manifests kustomize push-to-kind deploy

# UnDeploy controller from the configured Kubernetes cluster in ~/.kube/config
undeploy:
	$(KUSTOMIZE) build config/default | kubectl delete -f -

# Generate manifests e.g. CRD, RBAC etc.
manifests: controller-gen
	$(CONTROLLER_GEN) $(CRD_OPTIONS) rbac:roleName=manager-role webhook paths="./..." output:crd:artifacts:config=config/crd/bases

# Run crlfmt against code
fmt: crlfmt
	$(CRLFMT) -w -wrap=80 -ignore '_generated.deepcopy.go$$' .

# Run go vet against code
vet:
	go vet ./...

# Generate code
generate: controller-gen
	$(CONTROLLER_GEN) object:headerFile="../../../licenses/boilerplate.go.txt" paths="./..."

# Creates kind cluster
kind-create:
	(kind get clusters | grep kind && echo "kind cluster already exists") || (kind create cluster --config kind.yaml && echo "kind cluster created")

# Install cert-manager
certmanager-install: kind-create
	./hack/install-cert-manager.sh

prepare-dockerfile:
	echo "ARG BUILDPLATFORM" > Dockerfile
	cat Dockerfile.in >> Dockerfile

# Build the docker image
docker-build: prepare-dockerfile
	echo "~~~ Building operator image :docker:"
	docker build --build-arg BUILDPLATFORM='linux/${TARGETARCH}' --build-arg TARGETARCH=${TARGETARCH} --target=manager -f Dockerfile -t ${OPERATOR_IMG_LATEST} ../

# Build the docker image
docker-build-configurator: prepare-dockerfile
	echo "~~~ Building configurator image :docker:"
	docker build --build-arg BUILDPLATFORM='linux/${TARGETARCH}' --build-arg TARGETARCH=${TARGETARCH} --target=configurator -f Dockerfile -t ${CONFIGURATOR_IMG_LATEST} ../

# Preload controller image to kind cluster
push-to-kind: kind-create certmanager-install
	kind load docker-image ${OPERATOR_IMG_LATEST}
	kind load docker-image ${CONFIGURATOR_IMG_LATEST}

# Execute end to end tests
e2e-tests: kuttl test docker-build docker-build-configurator
	echo "~~~ Running kuttl tests :k8s:"
	$(KUTTL) test $(TEST_ONLY_FLAG) $(KUTTL_TEST_FLAGS)

# Execute end to end unstable tests
e2e-unstable-tests: kuttl test docker-build docker-build-configurator
	echo "~~~ Running kuttl unstable tests :k8s:"
	$(KUTTL) test --config kuttl-unstable-test.yaml --kind-context=${PR_NR:-kind} $(TEST_ONLY_FLAG) $(KUTTL_TEST_FLAGS)

# Execute end to end tests using helm as an installation
helm-e2e-tests: kuttl test docker-build docker-build-configurator
	echo "~~~ Running kuttl tests :k8s:"
	$(KUTTL) test --config kuttl-helm-test.yaml $(TEST_ONLY_FLAG) $(KUTTL_TEST_FLAGS)

# Download controller-gen locally if necessary
CONTROLLER_GEN = $(shell pwd)/bin/controller-gen
controller-gen:
	$(call go-get-tool,$(CONTROLLER_GEN),sigs.k8s.io/controller-tools/cmd/controller-gen@v0.4.1)

# Download kuttl locally if necessary
KUTTL = $(shell pwd)/bin/kubectl-kuttl
kuttl:
	$(call go-get-tool,$(KUTTL),github.com/kudobuilder/kuttl/cmd/kubectl-kuttl@v0.11.1)

# Download crlfmt locally if necessary
CRLFMT = $(shell pwd)/bin/crlfmt
crlfmt:
	$(call go-get-tool,$(CRLFMT),github.com/cockroachdb/crlfmt@v0.0.0-20210128092314-b3eff0b87c79)

# Download kustomize locally if necessary
KUSTOMIZE = $(shell pwd)/bin/kustomize
kustomize:
	$(call go-get-tool,$(KUSTOMIZE),sigs.k8s.io/kustomize/kustomize/v3@v3.8.7)

# go-get-tool will 'go get' any package $2 and install it to $1.
PROJECT_DIR := $(shell dirname $(abspath $(lastword $(MAKEFILE_LIST))))
define go-get-tool
@[ -f $(1) ] || { \
set -e ;\
TMP_DIR=$$(mktemp -d) ;\
cd $$TMP_DIR ;\
go mod init tmp ;\
echo "Downloading $(2)" ;\
GOBIN=$(PROJECT_DIR)/bin go get $(2) ;\
rm -rf $$TMP_DIR ;\
}
endef
