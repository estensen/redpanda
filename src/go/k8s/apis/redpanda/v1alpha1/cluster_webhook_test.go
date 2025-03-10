// Copyright 2021 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

package v1alpha1_test

import (
	"fmt"
	"strings"
	"testing"

	cmmeta "github.com/jetstack/cert-manager/pkg/apis/meta/v1"
	"github.com/redpanda-data/redpanda/src/go/k8s/apis/redpanda/v1alpha1"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/util/intstr"
	"k8s.io/apimachinery/pkg/util/validation/field"
	"k8s.io/utils/pointer"
)

// nolint:funlen // this is ok for a test
func TestDefault(t *testing.T) {
	type test struct {
		name                                string
		replicas                            int32
		additionalConfigurationSetByWebhook bool
		configAlreadyPresent                bool
	}
	tests := []test{
		{
			name:                                "do not set default topic replication when there is less than 3 replicas",
			replicas:                            2,
			additionalConfigurationSetByWebhook: false,
		},
		{
			name:                                "sets default topic replication",
			replicas:                            3,
			additionalConfigurationSetByWebhook: true,
		},
		{
			name:                                "does not set default topic replication when it already exists in CRD",
			replicas:                            3,
			additionalConfigurationSetByWebhook: false,
			configAlreadyPresent:                true,
		},
	}
	fields := []string{"redpanda.default_topic_replications", "redpanda.transaction_coordinator_replication", "redpanda.id_allocator_replication"}
	for _, tt := range tests {
		for _, field := range fields {
			t.Run(tt.name, func(t *testing.T) {
				redpandaCluster := &v1alpha1.Cluster{
					ObjectMeta: metav1.ObjectMeta{
						Name:      "test",
						Namespace: "",
					},
					Spec: v1alpha1.ClusterSpec{
						Replicas:      pointer.Int32Ptr(tt.replicas),
						Configuration: v1alpha1.RedpandaConfig{},
					},
				}

				if tt.configAlreadyPresent {
					redpandaCluster.Spec.AdditionalConfiguration = make(map[string]string)
					redpandaCluster.Spec.AdditionalConfiguration[field] = "111"
				}

				redpandaCluster.Default()
				val, exist := redpandaCluster.Spec.AdditionalConfiguration[field]
				if (exist && val == "3") != tt.additionalConfigurationSetByWebhook {
					t.Fail()
				}
			})
		}
	}

	t.Run("missing schema registry does not set default port", func(t *testing.T) {
		redpandaCluster := &v1alpha1.Cluster{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test",
				Namespace: "",
			},
			Spec: v1alpha1.ClusterSpec{
				Replicas:      pointer.Int32Ptr(1),
				Configuration: v1alpha1.RedpandaConfig{},
				Resources: v1alpha1.RedpandaResourceRequirements{
					ResourceRequirements: corev1.ResourceRequirements{
						Requests: corev1.ResourceList{
							corev1.ResourceMemory: resource.MustParse("1Gi"),
						},
					},
					Redpanda: nil,
				},
			},
		}

		redpandaCluster.Default()
		assert.Nil(t, redpandaCluster.Spec.Configuration.SchemaRegistry)
	})
	t.Run("if schema registry exist, but the port is 0 the default is set", func(t *testing.T) {
		redpandaCluster := &v1alpha1.Cluster{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test",
				Namespace: "",
			},
			Spec: v1alpha1.ClusterSpec{
				Replicas: pointer.Int32Ptr(1),
				Configuration: v1alpha1.RedpandaConfig{
					SchemaRegistry: &v1alpha1.SchemaRegistryAPI{},
				},
				Resources: v1alpha1.RedpandaResourceRequirements{
					ResourceRequirements: corev1.ResourceRequirements{
						Requests: corev1.ResourceList{
							corev1.ResourceMemory: resource.MustParse("1Gi"),
						},
					},
					Redpanda: nil,
				},
			},
		}

		redpandaCluster.Default()
		assert.Equal(t, 8081, redpandaCluster.Spec.Configuration.SchemaRegistry.Port)
	})
	t.Run("if schema registry exist and port have not zero value the default will not be used", func(t *testing.T) {
		redpandaCluster := &v1alpha1.Cluster{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test",
				Namespace: "",
			},
			Spec: v1alpha1.ClusterSpec{
				Replicas: pointer.Int32Ptr(1),
				Configuration: v1alpha1.RedpandaConfig{
					SchemaRegistry: &v1alpha1.SchemaRegistryAPI{
						Port: 999,
					},
				},
				Resources: v1alpha1.RedpandaResourceRequirements{
					ResourceRequirements: corev1.ResourceRequirements{
						Requests: corev1.ResourceList{
							corev1.ResourceMemory: resource.MustParse("1Gi"),
						},
					},
					Redpanda: nil,
				},
			},
		}

		redpandaCluster.Default()
		assert.Equal(t, 999, redpandaCluster.Spec.Configuration.SchemaRegistry.Port)
	})
	t.Run("if schema registry is defined as rest of external listeners the default port is used", func(t *testing.T) {
		redpandaCluster := &v1alpha1.Cluster{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test",
				Namespace: "",
			},
			Spec: v1alpha1.ClusterSpec{
				Replicas: pointer.Int32Ptr(1),
				Configuration: v1alpha1.RedpandaConfig{
					SchemaRegistry: &v1alpha1.SchemaRegistryAPI{
						External: &v1alpha1.ExternalConnectivityConfig{Enabled: true},
					},
				},
				Resources: v1alpha1.RedpandaResourceRequirements{
					ResourceRequirements: corev1.ResourceRequirements{
						Requests: corev1.ResourceList{
							corev1.ResourceMemory: resource.MustParse("1Gi"),
						},
					},
					Redpanda: nil,
				},
			},
		}

		redpandaCluster.Default()
		assert.Equal(t, 8081, redpandaCluster.Spec.Configuration.SchemaRegistry.Port)
	})
	t.Run("pod disruption budget", func(t *testing.T) {
		redpandaCluster := &v1alpha1.Cluster{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test",
				Namespace: "",
			},
			Spec: v1alpha1.ClusterSpec{
				Replicas: pointer.Int32Ptr(1),
			},
		}
		redpandaCluster.Default()
		assert.True(t, redpandaCluster.Spec.PodDisruptionBudget.Enabled)
		assert.Equal(t, intstr.FromInt(1), *redpandaCluster.Spec.PodDisruptionBudget.MaxUnavailable)
	})
}

func TestValidateUpdate(t *testing.T) {
	var replicas0 int32
	var replicas3 int32 = 3

	redpandaCluster := &v1alpha1.Cluster{
		ObjectMeta: metav1.ObjectMeta{
			Name:      "test",
			Namespace: "",
		},
		Spec: v1alpha1.ClusterSpec{
			Replicas:      pointer.Int32Ptr(replicas3),
			Configuration: v1alpha1.RedpandaConfig{},
			Resources: v1alpha1.RedpandaResourceRequirements{
				ResourceRequirements: corev1.ResourceRequirements{
					Requests: corev1.ResourceList{
						corev1.ResourceCPU:    resource.MustParse("1"),
						corev1.ResourceMemory: resource.MustParse("0.9Gi"),
					},
				},
				Redpanda: nil,
			},
		},
	}

	updatedCluster := redpandaCluster.DeepCopy()
	updatedCluster.Spec.Replicas = &replicas0
	updatedCluster.Spec.Configuration = v1alpha1.RedpandaConfig{
		KafkaAPI: []v1alpha1.KafkaAPI{
			{
				Port: 123,
				TLS: v1alpha1.KafkaAPITLS{
					RequireClientAuth: true,
					IssuerRef: &cmmeta.ObjectReference{
						Name: "test",
					},
					NodeSecretRef: &corev1.ObjectReference{
						Name:      "name",
						Namespace: "default",
					},
					Enabled: false,
				},
			},
		},
	}

	err := updatedCluster.ValidateUpdate(redpandaCluster)
	if err == nil {
		t.Fatalf("expecting validation error but got none")
	}

	// verify the error causes contain all expected fields
	statusError := err.(*apierrors.StatusError)
	expectedFields := []string{
		field.NewPath("spec").Child("replicas").String(),
		field.NewPath("spec").Child("resources").Child("requests").Child("memory").String(),
		field.NewPath("spec").Child("configuration").Child("kafkaApi").Index(0).Child("tls").Child("requireClientAuth").String(),
		field.NewPath("spec").Child("configuration").Child("kafkaApi").Index(0).Child("tls").Child("nodeSecretRef").String(),
	}

	for _, ef := range expectedFields {
		found := false
		for _, c := range statusError.Status().Details.Causes {
			if ef == c.Field {
				found = true
				break
			}
		}
		if !found {
			t.Errorf("expecting failure on field %s but have %v", ef, statusError.Status().Details.Causes)
		}
	}
}

func TestNilReplicasIsNotAllowed(t *testing.T) {
	rpCluster := validRedpandaCluster()
	err := rpCluster.ValidateCreate()
	require.Nil(t, err, "Initial cluster is not valid")
	rpCluster.Spec.Replicas = nil
	err = rpCluster.ValidateCreate()
	assert.Error(t, err)
}

//nolint:funlen // this is ok for a test
func TestValidateUpdate_NoError(t *testing.T) {
	var replicas2 int32 = 2

	redpandaCluster := &v1alpha1.Cluster{
		ObjectMeta: metav1.ObjectMeta{
			Name:      "test",
			Namespace: "",
		},
		Spec: v1alpha1.ClusterSpec{
			Replicas: pointer.Int32Ptr(replicas2),
			Configuration: v1alpha1.RedpandaConfig{
				KafkaAPI:       []v1alpha1.KafkaAPI{{Port: 124}},
				AdminAPI:       []v1alpha1.AdminAPI{{Port: 125}},
				RPCServer:      v1alpha1.SocketAddress{Port: 126},
				SchemaRegistry: &v1alpha1.SchemaRegistryAPI{Port: 127},
				PandaproxyAPI:  []v1alpha1.PandaproxyAPI{{Port: 128}},
			},
			Resources: v1alpha1.RedpandaResourceRequirements{
				ResourceRequirements: corev1.ResourceRequirements{
					Requests: corev1.ResourceList{
						corev1.ResourceMemory: resource.MustParse("2Gi"),
						corev1.ResourceCPU:    resource.MustParse("1"),
					},
				},
				Redpanda: nil,
			},
		},
	}

	t.Run("same object updated", func(t *testing.T) {
		err := redpandaCluster.ValidateUpdate(redpandaCluster)
		assert.NoError(t, err)
	})

	t.Run("scale up", func(t *testing.T) {
		scaleUp := *redpandaCluster.Spec.Replicas + 1
		updatedScaleUp := redpandaCluster.DeepCopy()
		updatedScaleUp.Spec.Replicas = &scaleUp
		err := updatedScaleUp.ValidateUpdate(redpandaCluster)
		assert.NoError(t, err)
	})

	t.Run("change image and tag", func(t *testing.T) {
		updatedImage := redpandaCluster.DeepCopy()
		updatedImage.Spec.Image = "differentimage"
		updatedImage.Spec.Version = "111"
		err := updatedImage.ValidateUpdate(redpandaCluster)
		assert.NoError(t, err)
	})

	t.Run("collision in the port", func(t *testing.T) {
		updatePort := redpandaCluster.DeepCopy()
		updatePort.Spec.Configuration.KafkaAPI[0].Port = 200
		updatePort.Spec.Configuration.AdminAPI[0].Port = 200
		updatePort.Spec.Configuration.RPCServer.Port = 200
		updatePort.Spec.Configuration.SchemaRegistry.Port = 200

		err := updatePort.ValidateUpdate(redpandaCluster)
		assert.Error(t, err)
	})

	t.Run("collision in the port when external connectivity is enabled", func(t *testing.T) {
		updatePort := redpandaCluster.DeepCopy()
		updatePort.Spec.Configuration.KafkaAPI = append(updatePort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		updatePort.Spec.Configuration.AdminAPI = append(updatePort.Spec.Configuration.AdminAPI,
			v1alpha1.AdminAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		updatePort.Spec.Configuration.PandaproxyAPI = append(updatePort.Spec.Configuration.PandaproxyAPI,
			v1alpha1.PandaproxyAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})

		err := updatePort.ValidateUpdate(redpandaCluster)
		assert.Error(t, err)
	})

	t.Run("no collision when schema registry has the next port to panda proxy", func(t *testing.T) {
		updatePort := redpandaCluster.DeepCopy()
		updatePort.Spec.Configuration.KafkaAPI[0].Port = 200
		updatePort.Spec.Configuration.KafkaAPI = append(updatePort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		updatePort.Spec.Configuration.PandaproxyAPI = append(updatePort.Spec.Configuration.PandaproxyAPI,
			v1alpha1.PandaproxyAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		updatePort.Spec.Configuration.SchemaRegistry.External = &v1alpha1.ExternalConnectivityConfig{
			Enabled: true,
		}

		err := updatePort.ValidateUpdate(redpandaCluster)
		assert.NoError(t, err)
	})

	t.Run("collision in the port when external connectivity is enabled", func(t *testing.T) {
		updatePort := redpandaCluster.DeepCopy()
		updatePort.Spec.Configuration.KafkaAPI = append(updatePort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		updatePort.Spec.Configuration.KafkaAPI[0].Port = 200
		updatePort.Spec.Configuration.AdminAPI[0].Port = 300
		updatePort.Spec.Configuration.RPCServer.Port = 201

		err := updatePort.ValidateUpdate(redpandaCluster)
		assert.Error(t, err)
	})

	t.Run("collision in the port when external connectivity is enabled", func(t *testing.T) {
		updatePort := redpandaCluster.DeepCopy()
		updatePort.Spec.Configuration.KafkaAPI = append(updatePort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		updatePort.Spec.Configuration.KafkaAPI[0].Port = 200
		updatePort.Spec.Configuration.AdminAPI[0].Port = 300
		updatePort.Spec.Configuration.AdminAPI[0].External.Enabled = true
		updatePort.Spec.Configuration.RPCServer.Port = 301

		err := updatePort.ValidateUpdate(redpandaCluster)
		assert.Error(t, err)
	})

	t.Run("port collision with proxy and schema registry", func(t *testing.T) {
		updatePort := redpandaCluster.DeepCopy()
		updatePort.Spec.Configuration.SchemaRegistry.Port = updatePort.Spec.Configuration.PandaproxyAPI[0].Port

		err := updatePort.ValidateUpdate(redpandaCluster)
		assert.Error(t, err)
	})

	t.Run("collision in admin port when external connectivity is enabled", func(t *testing.T) {
		updatePort := redpandaCluster.DeepCopy()
		updatePort.Spec.Configuration.AdminAPI[0].External.Enabled = true
		updatePort.Spec.Configuration.KafkaAPI = append(updatePort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		updatePort.Spec.Configuration.KafkaAPI[0].Port = 201
		updatePort.Spec.Configuration.AdminAPI[0].Port = 200
		updatePort.Spec.Configuration.RPCServer.Port = 300

		err := updatePort.ValidateUpdate(redpandaCluster)
		assert.Error(t, err)
	})

	t.Run("requireclientauth true and tls enabled", func(t *testing.T) {
		tls := redpandaCluster.DeepCopy()
		tls.Spec.Configuration.KafkaAPI[0].TLS.RequireClientAuth = true
		tls.Spec.Configuration.KafkaAPI[0].TLS.Enabled = true

		err := tls.ValidateUpdate(redpandaCluster)
		assert.NoError(t, err)
	})

	t.Run("multiple external listeners", func(t *testing.T) {
		exPort := redpandaCluster.DeepCopy()
		exPort.Spec.Configuration.KafkaAPI[0].External.Enabled = true
		exPort.Spec.Configuration.KafkaAPI = append(exPort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{Port: 123, External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		err := exPort.ValidateUpdate(redpandaCluster)

		assert.Error(t, err)
	})

	t.Run("multiple internal listeners", func(t *testing.T) {
		multiPort := redpandaCluster.DeepCopy()
		multiPort.Spec.Configuration.KafkaAPI = append(multiPort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{Port: 123})
		err := multiPort.ValidateUpdate(redpandaCluster)

		assert.Error(t, err)
	})

	t.Run("external listener cannot have port specified", func(t *testing.T) {
		exPort := redpandaCluster.DeepCopy()
		exPort.Spec.Configuration.KafkaAPI[0].External.Enabled = true
		err := exPort.ValidateUpdate(redpandaCluster)

		assert.Error(t, err)
	})

	t.Run("no admin port", func(t *testing.T) {
		noPort := redpandaCluster.DeepCopy()
		noPort.Spec.Configuration.AdminAPI = []v1alpha1.AdminAPI{}

		err := noPort.ValidateUpdate(redpandaCluster)
		assert.Error(t, err)
	})

	t.Run("multiple internal admin listeners", func(t *testing.T) {
		multiPort := redpandaCluster.DeepCopy()
		multiPort.Spec.Configuration.AdminAPI = append(multiPort.Spec.Configuration.AdminAPI,
			v1alpha1.AdminAPI{Port: 123})
		err := multiPort.ValidateUpdate(redpandaCluster)

		assert.Error(t, err)
	})

	t.Run("multiple admin listeners with tls", func(t *testing.T) {
		multiPort := redpandaCluster.DeepCopy()
		multiPort.Spec.Configuration.AdminAPI[0].TLS.Enabled = true
		multiPort.Spec.Configuration.AdminAPI = append(multiPort.Spec.Configuration.AdminAPI,
			v1alpha1.AdminAPI{
				Port:     123,
				External: v1alpha1.ExternalConnectivityConfig{Enabled: true},
				TLS:      v1alpha1.AdminAPITLS{Enabled: true},
			})
		err := multiPort.ValidateUpdate(redpandaCluster)

		assert.Error(t, err)
	})

	t.Run("tls admin listener without enabled true", func(t *testing.T) {
		multiPort := redpandaCluster.DeepCopy()
		multiPort.Spec.Configuration.AdminAPI[0].TLS.RequireClientAuth = true
		multiPort.Spec.Configuration.AdminAPI[0].TLS.Enabled = false
		err := multiPort.ValidateUpdate(redpandaCluster)

		assert.Error(t, err)
	})

	t.Run("proxy subdomain must be the same as kafka subdomain", func(t *testing.T) {
		withSub := redpandaCluster.DeepCopy()
		withSub.Spec.Configuration.PandaproxyAPI = []v1alpha1.PandaproxyAPI{
			{
				Port:     145,
				External: v1alpha1.ExternalConnectivityConfig{Enabled: true, Subdomain: "subdomain"},
			},
		}
		err := withSub.ValidateUpdate(redpandaCluster)

		assert.Error(t, err)
	})

	t.Run("cannot have multiple internal proxy listeners", func(t *testing.T) {
		multiPort := redpandaCluster.DeepCopy()
		multiPort.Spec.Configuration.PandaproxyAPI = append(multiPort.Spec.Configuration.PandaproxyAPI,
			v1alpha1.PandaproxyAPI{Port: 123}, v1alpha1.PandaproxyAPI{Port: 321})
		err := multiPort.ValidateUpdate(redpandaCluster)

		assert.Error(t, err)
	})

	t.Run("cannot have external proxy listener without an internal one", func(t *testing.T) {
		noInternal := redpandaCluster.DeepCopy()
		noInternal.Spec.Configuration.PandaproxyAPI = append(noInternal.Spec.Configuration.PandaproxyAPI,
			v1alpha1.PandaproxyAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}, Port: 123})
		err := noInternal.ValidateUpdate(redpandaCluster)

		assert.Error(t, err)
	})

	t.Run("external proxy listener cannot have port specified", func(t *testing.T) {
		multiPort := redpandaCluster.DeepCopy()
		multiPort.Spec.Configuration.PandaproxyAPI = append(multiPort.Spec.Configuration.PandaproxyAPI,
			v1alpha1.PandaproxyAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}, Port: 123},
			v1alpha1.PandaproxyAPI{Port: 321})
		err := multiPort.ValidateUpdate(redpandaCluster)

		assert.Error(t, err)
	})

	t.Run("pandaproxy tls disabled with client auth enabled", func(t *testing.T) {
		tls := redpandaCluster.DeepCopy()
		tls.Spec.Configuration.PandaproxyAPI = append(tls.Spec.Configuration.PandaproxyAPI,
			v1alpha1.PandaproxyAPI{TLS: v1alpha1.PandaproxyAPITLS{Enabled: false, RequireClientAuth: true}})

		err := tls.ValidateUpdate(redpandaCluster)
		assert.Error(t, err)
	})

	t.Run("resource limits/requests on redpanda resources", func(t *testing.T) {
		c := redpandaCluster.DeepCopy()
		c.Spec.Resources.Limits = corev1.ResourceList{
			corev1.ResourceMemory: resource.MustParse("1Gi"),
			corev1.ResourceCPU:    resource.MustParse("1"),
		}
		c.Spec.Resources.Requests = corev1.ResourceList{
			corev1.ResourceMemory: resource.MustParse("2Gi"),
			corev1.ResourceCPU:    resource.MustParse("1"),
		}

		err := c.ValidateUpdate(redpandaCluster)
		assert.Error(t, err)
	})

	t.Run("resource limits/requests on rpk status resources", func(t *testing.T) {
		c := redpandaCluster.DeepCopy()
		c.Spec.Sidecars = v1alpha1.Sidecars{
			RpkStatus: &v1alpha1.Sidecar{
				Enabled: true,
				Resources: &corev1.ResourceRequirements{
					Limits: corev1.ResourceList{
						corev1.ResourceMemory: resource.MustParse("1Gi"),
						corev1.ResourceCPU:    resource.MustParse("1"),
					},
					Requests: corev1.ResourceList{
						corev1.ResourceMemory: resource.MustParse("2Gi"),
						corev1.ResourceCPU:    resource.MustParse("1"),
					},
				},
			},
		}

		err := c.ValidateUpdate(redpandaCluster)
		assert.Error(t, err)
	})

	decreaseCases := []struct {
		initial    string
		target     string
		error      bool
		lowerBound string
	}{
		{
			initial: "2",
			target:  "2500m",
			error:   false,
		},
		{
			initial: "2",
			target:  "1001m",
			error:   false,
		},
		{
			initial:    "2000m",
			target:     "999m",
			error:      true,
			lowerBound: "1001m",
		},
		{
			initial:    "1.1",
			target:     "1",
			error:      true,
			lowerBound: "1001m",
		},
	}
	for _, tc := range decreaseCases {
		t.Run(fmt.Sprintf("CPU request change from %s to %s", tc.initial, tc.target), func(t *testing.T) {
			oldCluster := redpandaCluster.DeepCopy()
			oldCluster.Spec.Resources.Requests = corev1.ResourceList{
				corev1.ResourceMemory: resource.MustParse("20Gi"),
				corev1.ResourceCPU:    resource.MustParse(tc.initial),
			}
			oldCluster.Spec.Resources.Limits = corev1.ResourceList{
				corev1.ResourceMemory: resource.MustParse("20Gi"),
				corev1.ResourceCPU:    resource.MustParse(tc.initial),
			}

			newCluster := redpandaCluster.DeepCopy()
			newCluster.Spec.Resources.Requests = corev1.ResourceList{
				corev1.ResourceMemory: resource.MustParse("20Gi"),
				corev1.ResourceCPU:    resource.MustParse(tc.target),
			}

			err := newCluster.ValidateUpdate(oldCluster)
			if tc.error {
				assert.Error(t, err)
				if err != nil && tc.lowerBound != "" {
					parts := strings.Split(err.Error(), " ")
					computedBound := parts[len(parts)-1]
					assert.Equal(t, tc.lowerBound, computedBound)
				}
			} else {
				assert.NoError(t, err)
			}
		})
	}
}

//nolint:funlen // this is ok for a test
func TestCreation(t *testing.T) {
	redpandaCluster := validRedpandaCluster()

	t.Run("no collision in the port", func(t *testing.T) {
		newPort := redpandaCluster.DeepCopy()
		newPort.Spec.Configuration.KafkaAPI[0].Port = 200

		err := newPort.ValidateCreate()
		assert.NoError(t, err)
	})

	t.Run("collision in the port", func(t *testing.T) {
		newPort := redpandaCluster.DeepCopy()
		newPort.Spec.Configuration.KafkaAPI[0].Port = 200
		newPort.Spec.Configuration.AdminAPI[0].Port = 200
		newPort.Spec.Configuration.RPCServer.Port = 200
		newPort.Spec.Configuration.SchemaRegistry.Port = 200

		err := newPort.ValidateCreate()
		assert.Error(t, err)
	})

	t.Run("collision in the port when external connectivity is enabled", func(t *testing.T) {
		newPort := redpandaCluster.DeepCopy()
		newPort.Spec.Configuration.AdminAPI[0].Port = newPort.Spec.Configuration.KafkaAPI[0].Port + 1
		newPort.Spec.Configuration.KafkaAPI = append(newPort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		newPort.Spec.Configuration.AdminAPI = append(newPort.Spec.Configuration.AdminAPI,
			v1alpha1.AdminAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		newPort.Spec.Configuration.PandaproxyAPI = append(newPort.Spec.Configuration.PandaproxyAPI,
			v1alpha1.PandaproxyAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})

		err := newPort.ValidateCreate()
		assert.Error(t, err)
	})

	t.Run("no collision when schema registry has the next port to panda proxy", func(t *testing.T) {
		newPort := redpandaCluster.DeepCopy()
		newPort.Spec.Configuration.KafkaAPI[0].Port = 200
		newPort.Spec.Configuration.KafkaAPI = append(newPort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		newPort.Spec.Configuration.PandaproxyAPI = append(newPort.Spec.Configuration.PandaproxyAPI,
			v1alpha1.PandaproxyAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		newPort.Spec.Configuration.SchemaRegistry.External = &v1alpha1.ExternalConnectivityConfig{
			Enabled: true,
		}

		err := newPort.ValidateCreate()
		assert.NoError(t, err)
	})

	t.Run("port collision with proxy and schema registry", func(t *testing.T) {
		newPort := redpandaCluster.DeepCopy()
		newPort.Spec.Configuration.SchemaRegistry.Port = newPort.Spec.Configuration.PandaproxyAPI[0].Port

		err := newPort.ValidateCreate()
		assert.Error(t, err)
	})

	t.Run("no kafka port", func(t *testing.T) {
		noPort := redpandaCluster.DeepCopy()
		noPort.Spec.Configuration.KafkaAPI = []v1alpha1.KafkaAPI{}

		err := noPort.ValidateCreate()
		assert.Error(t, err)
	})

	t.Run("no admin port", func(t *testing.T) {
		noPort := redpandaCluster.DeepCopy()
		noPort.Spec.Configuration.AdminAPI = []v1alpha1.AdminAPI{}

		err := noPort.ValidateCreate()
		assert.Error(t, err)
	})

	t.Run("multiple internal admin listeners", func(t *testing.T) {
		multiPort := redpandaCluster.DeepCopy()
		multiPort.Spec.Configuration.AdminAPI = append(multiPort.Spec.Configuration.AdminAPI,
			v1alpha1.AdminAPI{Port: 123})
		err := multiPort.ValidateCreate()

		assert.Error(t, err)
	})

	t.Run("incorrect memory (need 2GB per core)", func(t *testing.T) {
		memory := redpandaCluster.DeepCopy()
		memory.Spec.Resources = v1alpha1.RedpandaResourceRequirements{
			ResourceRequirements: corev1.ResourceRequirements{
				Requests: corev1.ResourceList{
					corev1.ResourceMemory: resource.MustParse("1Gi"),
					corev1.ResourceCPU:    resource.MustParse("2"),
				},
			},
			Redpanda: nil,
		}

		err := memory.ValidateCreate()
		assert.Error(t, err)
	})

	t.Run("no 2GB per core required when in developer mode", func(t *testing.T) {
		memory := redpandaCluster.DeepCopy()
		memory.Spec.Resources = v1alpha1.RedpandaResourceRequirements{
			ResourceRequirements: corev1.ResourceRequirements{
				Requests: corev1.ResourceList{
					corev1.ResourceMemory: resource.MustParse("1Gi"),
					corev1.ResourceCPU:    resource.MustParse("2"),
				},
			},
			Redpanda: nil,
		}
		memory.Spec.Configuration.DeveloperMode = true

		err := memory.ValidateCreate()
		assert.NoError(t, err)
	})

	t.Run("incorrect redpanda memory (need <= request)", func(t *testing.T) {
		memory := redpandaCluster.DeepCopy()
		memory.Spec.Resources = v1alpha1.RedpandaResourceRequirements{
			ResourceRequirements: corev1.ResourceRequirements{
				Requests: corev1.ResourceList{
					corev1.ResourceMemory: resource.MustParse("2Gi"),
					corev1.ResourceCPU:    resource.MustParse("1"),
				},
			},
			Redpanda: corev1.ResourceList{
				corev1.ResourceMemory: resource.MustParse("4Gi"),
				corev1.ResourceCPU:    resource.MustParse("1"),
			},
		}

		err := memory.ValidateCreate()
		assert.Error(t, err)
	})

	// nolint:dupl // the values are different
	t.Run("incorrect redpanda memory (need <= limit)", func(t *testing.T) {
		memory := redpandaCluster.DeepCopy()
		memory.Spec.Resources = v1alpha1.RedpandaResourceRequirements{
			ResourceRequirements: corev1.ResourceRequirements{
				Requests: corev1.ResourceList{
					corev1.ResourceMemory: resource.MustParse("2Gi"),
					corev1.ResourceCPU:    resource.MustParse("1"),
				},
				Limits: corev1.ResourceList{
					corev1.ResourceMemory: resource.MustParse("3Gi"),
					corev1.ResourceCPU:    resource.MustParse("1"),
				},
			},
			Redpanda: corev1.ResourceList{
				corev1.ResourceMemory: resource.MustParse("4Gi"),
				corev1.ResourceCPU:    resource.MustParse("1"),
			},
		}

		err := memory.ValidateCreate()
		assert.Error(t, err)
	})

	// nolint:dupl // the values are different
	t.Run("correct redpanda memory", func(t *testing.T) {
		memory := redpandaCluster.DeepCopy()
		memory.Spec.Resources = v1alpha1.RedpandaResourceRequirements{
			ResourceRequirements: corev1.ResourceRequirements{
				Requests: corev1.ResourceList{
					corev1.ResourceMemory: resource.MustParse("2.223Gi"),
					corev1.ResourceCPU:    resource.MustParse("1"),
				},
				Limits: corev1.ResourceList{
					corev1.ResourceMemory: resource.MustParse("2.223Gi"),
					corev1.ResourceCPU:    resource.MustParse("1"),
				},
			},
			Redpanda: corev1.ResourceList{
				corev1.ResourceMemory: resource.MustParse("2Gi"),
				corev1.ResourceCPU:    resource.MustParse("1"),
			},
		}

		err := memory.ValidateCreate()
		assert.NoError(t, err)
	})

	// nolint:dupl // the values are different
	t.Run("correct redpanda memory (boundary check)", func(t *testing.T) {
		memory := redpandaCluster.DeepCopy()
		memory.Spec.Resources = v1alpha1.RedpandaResourceRequirements{
			ResourceRequirements: corev1.ResourceRequirements{
				Requests: corev1.ResourceList{
					corev1.ResourceMemory: resource.MustParse("2Gi"),
					corev1.ResourceCPU:    resource.MustParse("1"),
				},
				Limits: corev1.ResourceList{
					corev1.ResourceMemory: resource.MustParse("2Gi"),
					corev1.ResourceCPU:    resource.MustParse("1"),
				},
			},
			Redpanda: corev1.ResourceList{
				corev1.ResourceMemory: resource.MustParse("2Gi"),
				corev1.ResourceCPU:    resource.MustParse("1"),
			},
		}

		err := memory.ValidateCreate()
		assert.NoError(t, err)
	})

	t.Run("tls properly configured", func(t *testing.T) {
		tls := redpandaCluster.DeepCopy()
		tls.Spec.Configuration.KafkaAPI[0].TLS.Enabled = true
		tls.Spec.Configuration.KafkaAPI[0].TLS.RequireClientAuth = true

		err := tls.ValidateCreate()
		assert.NoError(t, err)
	})

	t.Run("require client auth without tls enabled", func(t *testing.T) {
		tls := redpandaCluster.DeepCopy()
		tls.Spec.Configuration.KafkaAPI[0].TLS.Enabled = false
		tls.Spec.Configuration.KafkaAPI[0].TLS.RequireClientAuth = true

		err := tls.ValidateCreate()
		assert.Error(t, err)
	})

	t.Run("multiple external listeners", func(t *testing.T) {
		exPort := redpandaCluster.DeepCopy()
		exPort.Spec.Configuration.KafkaAPI[0].External.Enabled = true
		exPort.Spec.Configuration.KafkaAPI = append(exPort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{Port: 123, External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		err := exPort.ValidateCreate()

		assert.Error(t, err)
	})

	t.Run("multiple internal listeners", func(t *testing.T) {
		multiPort := redpandaCluster.DeepCopy()
		multiPort.Spec.Configuration.KafkaAPI = append(multiPort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{Port: 123})
		err := multiPort.ValidateCreate()

		assert.Error(t, err)
	})

	t.Run("external listener with port", func(t *testing.T) {
		exPort := redpandaCluster.DeepCopy()
		exPort.Spec.Configuration.KafkaAPI[0].External.Enabled = true
		err := exPort.ValidateCreate()

		assert.Error(t, err)
	})

	t.Run("multiple admin listeners with tls", func(t *testing.T) {
		multiPort := redpandaCluster.DeepCopy()
		multiPort.Spec.Configuration.AdminAPI[0].TLS.Enabled = true
		multiPort.Spec.Configuration.AdminAPI = append(multiPort.Spec.Configuration.AdminAPI,
			v1alpha1.AdminAPI{
				Port:     123,
				External: v1alpha1.ExternalConnectivityConfig{Enabled: true},
				TLS:      v1alpha1.AdminAPITLS{Enabled: true},
			})
		err := multiPort.ValidateCreate()

		assert.Error(t, err)
	})

	t.Run("tls admin listener without enabled true", func(t *testing.T) {
		multiPort := redpandaCluster.DeepCopy()
		multiPort.Spec.Configuration.AdminAPI[0].TLS.RequireClientAuth = true
		multiPort.Spec.Configuration.AdminAPI[0].TLS.Enabled = false
		err := multiPort.ValidateCreate()

		assert.Error(t, err)
	})

	t.Run("proxy subdomain must be the same as kafka subdomain", func(t *testing.T) {
		withSub := redpandaCluster.DeepCopy()
		withSub.Spec.Configuration.PandaproxyAPI = []v1alpha1.PandaproxyAPI{
			{
				Port:     145,
				External: v1alpha1.ExternalConnectivityConfig{Enabled: true, Subdomain: "subdomain"},
			},
		}
		err := withSub.ValidateCreate()

		assert.Error(t, err)
	})

	t.Run("cannot have multiple internal proxy listeners", func(t *testing.T) {
		multiPort := redpandaCluster.DeepCopy()
		multiPort.Spec.Configuration.PandaproxyAPI = append(multiPort.Spec.Configuration.PandaproxyAPI,
			v1alpha1.PandaproxyAPI{Port: 123}, v1alpha1.PandaproxyAPI{Port: 321})
		err := multiPort.ValidateCreate()

		assert.Error(t, err)
	})

	t.Run("cannot have external proxy listener without an internal one", func(t *testing.T) {
		noInternal := redpandaCluster.DeepCopy()
		noInternal.Spec.Configuration.PandaproxyAPI = append(noInternal.Spec.Configuration.PandaproxyAPI,
			v1alpha1.PandaproxyAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}, Port: 123})
		err := noInternal.ValidateCreate()

		assert.Error(t, err)
	})

	t.Run("external proxy listener cannot have port specified", func(t *testing.T) {
		multiPort := redpandaCluster.DeepCopy()
		multiPort.Spec.Configuration.PandaproxyAPI = append(multiPort.Spec.Configuration.PandaproxyAPI,
			v1alpha1.PandaproxyAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true}, Port: 123},
			v1alpha1.PandaproxyAPI{Port: 321})
		err := multiPort.ValidateCreate()

		assert.Error(t, err)
	})

	t.Run("pandaproxy tls disabled but client auth enabled", func(t *testing.T) {
		tls := redpandaCluster.DeepCopy()
		tls.Spec.Configuration.PandaproxyAPI = append(tls.Spec.Configuration.PandaproxyAPI,
			v1alpha1.PandaproxyAPI{TLS: v1alpha1.PandaproxyAPITLS{Enabled: false, RequireClientAuth: true}})

		err := tls.ValidateCreate()
		assert.Error(t, err)
	})
	t.Run("kafka external subdomain is provided along with preferred address type", func(t *testing.T) {
		rp := redpandaCluster.DeepCopy()
		rp.Spec.Configuration.KafkaAPI = append(rp.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{Port: 123, External: v1alpha1.ExternalConnectivityConfig{Enabled: true, PreferredAddressType: "preferred", Subdomain: "subdomain"}})
		err := rp.ValidateCreate()
		assert.Error(t, err)
	})
	// No support for IP-based TLS certs (#2256)
	t.Run("kafka TLS for external listener without a subdomain", func(t *testing.T) {
		rp := redpandaCluster.DeepCopy()
		rp.Spec.Configuration.KafkaAPI = append(rp.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{
				TLS: v1alpha1.KafkaAPITLS{
					Enabled: true,
				},
				Port: 123, External: v1alpha1.ExternalConnectivityConfig{Enabled: true, PreferredAddressType: "InternalIP"},
			})
		err := rp.ValidateCreate()
		assert.Error(t, err)
	})
	t.Run("bootstrap loadbalancer for kafka api needs a port", func(t *testing.T) {
		rp := redpandaCluster.DeepCopy()
		rp.Spec.Configuration.KafkaAPI = append(rp.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true, Bootstrap: &v1alpha1.LoadBalancerConfig{}}})
		err := rp.ValidateCreate()
		assert.Error(t, err)
	})
	t.Run("bootstrap loadbalancer not allowed for admin", func(t *testing.T) {
		rp := redpandaCluster.DeepCopy()
		rp.Spec.Configuration.AdminAPI = append(rp.Spec.Configuration.AdminAPI,
			v1alpha1.AdminAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true, Bootstrap: &v1alpha1.LoadBalancerConfig{
				Port: 123,
			}}})
		err := rp.ValidateCreate()
		assert.Error(t, err)
	})
	t.Run("bootstrap loadbalancer not allowed for pandaproxy", func(t *testing.T) {
		rp := redpandaCluster.DeepCopy()
		rp.Spec.Configuration.PandaproxyAPI = append(rp.Spec.Configuration.PandaproxyAPI,
			v1alpha1.PandaproxyAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true, Bootstrap: &v1alpha1.LoadBalancerConfig{
				Port: 123,
			}}})
		err := rp.ValidateCreate()
		assert.Error(t, err)
	})
	t.Run("bootstrap loadbalancer not allowed for schemaregistry", func(t *testing.T) {
		rp := redpandaCluster.DeepCopy()
		rp.Spec.Configuration.SchemaRegistry = &v1alpha1.SchemaRegistryAPI{External: &v1alpha1.ExternalConnectivityConfig{Enabled: true, Bootstrap: &v1alpha1.LoadBalancerConfig{
			Port: 123,
		}}}
		err := rp.ValidateCreate()
		assert.Error(t, err)
	})
}

func TestSchemaRegistryValidations(t *testing.T) {
	redpandaCluster := validRedpandaCluster()

	t.Run("if schema registry externally available, kafka external listener is required", func(t *testing.T) {
		schemaReg := redpandaCluster.DeepCopy()
		schemaReg.Spec.Configuration.SchemaRegistry = &v1alpha1.SchemaRegistryAPI{
			External: &v1alpha1.ExternalConnectivityConfig{Enabled: true},
		}
		schemaReg.Spec.Configuration.KafkaAPI[0].External.Enabled = false

		err := schemaReg.ValidateCreate()
		assert.Error(t, err)
	})

	t.Run("schema registry externally available is valid when it has the same subdomain as kafka external listener", func(t *testing.T) {
		schemaReg := redpandaCluster.DeepCopy()
		schemaReg.Spec.Configuration.SchemaRegistry = &v1alpha1.SchemaRegistryAPI{
			External: &v1alpha1.ExternalConnectivityConfig{Enabled: true, Subdomain: "test.com"},
		}
		schemaReg.Spec.Configuration.KafkaAPI = append(schemaReg.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true, Subdomain: "test.com"}})

		err := schemaReg.ValidateCreate()
		assert.NoError(t, err)
	})

	t.Run("if schema registry externally available, it should have same subdomain as kafka external listener", func(t *testing.T) {
		schemaReg := redpandaCluster.DeepCopy()
		schemaReg.Spec.Configuration.SchemaRegistry = &v1alpha1.SchemaRegistryAPI{
			External: &v1alpha1.ExternalConnectivityConfig{Enabled: true, Subdomain: "test.com"},
		}
		schemaReg.Spec.Configuration.KafkaAPI = append(schemaReg.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true, Subdomain: "other.com"}})

		err := schemaReg.ValidateCreate()
		assert.Error(t, err)
	})
	t.Run("if schema registry externally available, kafka external listener should not be empty", func(t *testing.T) {
		schemaReg := redpandaCluster.DeepCopy()
		schemaReg.Spec.Configuration.SchemaRegistry = &v1alpha1.SchemaRegistryAPI{
			External: &v1alpha1.ExternalConnectivityConfig{Enabled: true, Subdomain: "test.com"},
		}
		schemaReg.Spec.Configuration.KafkaAPI = append(schemaReg.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{External: v1alpha1.ExternalConnectivityConfig{Enabled: true, Subdomain: ""}})

		err := schemaReg.ValidateCreate()
		assert.Error(t, err)
	})

	t.Run("schema registry mTLS enabled and TLS enabled is valid", func(t *testing.T) {
		schemaReg := redpandaCluster.DeepCopy()
		schemaReg.Spec.Configuration.SchemaRegistry = &v1alpha1.SchemaRegistryAPI{
			TLS: &v1alpha1.SchemaRegistryAPITLS{
				Enabled:           true,
				RequireClientAuth: true,
			},
		}

		err := schemaReg.ValidateCreate()
		assert.NoError(t, err)
	})

	t.Run("if schema registry mTLS enabled, TLS should also be enabled", func(t *testing.T) {
		schemaReg := redpandaCluster.DeepCopy()
		schemaReg.Spec.Configuration.SchemaRegistry = &v1alpha1.SchemaRegistryAPI{
			TLS: &v1alpha1.SchemaRegistryAPITLS{
				Enabled:           false,
				RequireClientAuth: true,
			},
		}

		err := schemaReg.ValidateCreate()
		assert.Error(t, err)
	})
}

func validRedpandaCluster() *v1alpha1.Cluster {
	return &v1alpha1.Cluster{
		ObjectMeta: metav1.ObjectMeta{
			Name:      "test",
			Namespace: "",
		},
		Spec: v1alpha1.ClusterSpec{
			Replicas: pointer.Int32Ptr(1),
			Configuration: v1alpha1.RedpandaConfig{
				KafkaAPI:       []v1alpha1.KafkaAPI{{Port: 124}},
				AdminAPI:       []v1alpha1.AdminAPI{{Port: 126}},
				RPCServer:      v1alpha1.SocketAddress{Port: 128},
				SchemaRegistry: &v1alpha1.SchemaRegistryAPI{Port: 130},
				PandaproxyAPI:  []v1alpha1.PandaproxyAPI{{Port: 132}},
			},
			Resources: v1alpha1.RedpandaResourceRequirements{
				ResourceRequirements: corev1.ResourceRequirements{
					Requests: corev1.ResourceList{
						corev1.ResourceMemory: resource.MustParse("2Gi"),
						corev1.ResourceCPU:    resource.MustParse("1"),
					},
				},
				Redpanda: nil,
			},
		},
	}
}

func TestPodDisruptionBudget(t *testing.T) {
	rpCluster := validRedpandaCluster()
	value := intstr.FromInt(1)

	t.Run("pdb not enabled is valid", func(t *testing.T) {
		rpc := rpCluster.DeepCopy()
		rpc.Spec.PodDisruptionBudget = &v1alpha1.PDBConfig{
			Enabled: false,
		}

		err := rpc.ValidateCreate()
		assert.NoError(t, err)
	})

	t.Run("pdb with only maxunavailable is valid", func(t *testing.T) {
		rpc := rpCluster.DeepCopy()
		rpc.Spec.PodDisruptionBudget = &v1alpha1.PDBConfig{
			Enabled:        true,
			MaxUnavailable: &value,
		}

		err := rpc.ValidateCreate()
		assert.NoError(t, err)
	})

	t.Run("pdb with only minavailable is valid", func(t *testing.T) {
		rpc := rpCluster.DeepCopy()
		rpc.Spec.PodDisruptionBudget = &v1alpha1.PDBConfig{
			Enabled:      true,
			MinAvailable: &value,
		}

		err := rpc.ValidateCreate()
		assert.NoError(t, err)
	})

	t.Run("pdb with both minavailable and maxunavailable is invalid", func(t *testing.T) {
		rpc := rpCluster.DeepCopy()
		rpc.Spec.PodDisruptionBudget = &v1alpha1.PDBConfig{
			Enabled:        true,
			MinAvailable:   &value,
			MaxUnavailable: &value,
		}

		err := rpc.ValidateCreate()
		assert.Error(t, err)
	})

	t.Run("pdb with minavailable but enabled=false is invalid", func(t *testing.T) {
		rpc := rpCluster.DeepCopy()
		rpc.Spec.PodDisruptionBudget = &v1alpha1.PDBConfig{
			Enabled:      false,
			MinAvailable: &value,
		}

		err := rpc.ValidateCreate()
		assert.Error(t, err)
	})

	t.Run("pdb is nil", func(t *testing.T) {
		// this can happen only if webhook is disabled
		rpc := rpCluster.DeepCopy()
		rpc.Spec.PodDisruptionBudget = nil

		err := rpc.ValidateCreate()
		assert.NoError(t, err)
	})
}

func TestExternalKafkaPortSpecified(t *testing.T) {
	rpCluster := validRedpandaCluster()

	t.Run("collision in the port when kafka api external port is defined", func(t *testing.T) {
		updatePort := rpCluster.DeepCopy()
		updatePort.Spec.Configuration.KafkaAPI = append(updatePort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{Port: 30001, External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		updatePort.Spec.Configuration.AdminAPI = []v1alpha1.AdminAPI{{Port: 30001}}

		err := updatePort.ValidateUpdate(updatePort)
		assert.Error(t, err)
	})

	t.Run("no collision in the port when kafka api external port is defined", func(t *testing.T) {
		updatePort := rpCluster.DeepCopy()
		updatePort.Spec.Configuration.KafkaAPI = append(updatePort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{Port: 30001, External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})
		updatePort.Spec.Configuration.AdminAPI = []v1alpha1.AdminAPI{{Port: 30002}}

		err := updatePort.ValidateUpdate(updatePort)
		assert.NoError(t, err)
	})

	t.Run("error when kafkaAPI external port is outside of supported range", func(t *testing.T) {
		updatePort := rpCluster.DeepCopy()
		updatePort.Spec.Configuration.KafkaAPI = append(updatePort.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{Port: 29999, External: v1alpha1.ExternalConnectivityConfig{Enabled: true}})

		err := updatePort.ValidateUpdate(updatePort)
		assert.Error(t, err)
	})
}

func TestKafkaTLSRules(t *testing.T) {
	rpCluster := validRedpandaCluster()

	// nolint:dupl // the tests are not duplicates
	t.Run("different issuer for two tls listeners", func(t *testing.T) {
		newRp := rpCluster.DeepCopy()
		newRp.Spec.Configuration.KafkaAPI[0].TLS = v1alpha1.KafkaAPITLS{
			Enabled: true,
			IssuerRef: &cmmeta.ObjectReference{
				Name: "issuer",
				Kind: "ClusterIssuer",
			},
		}
		newRp.Spec.Configuration.KafkaAPI = append(newRp.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{Port: 30001, External: v1alpha1.ExternalConnectivityConfig{Enabled: true, Subdomain: "redpanda.com"}, TLS: v1alpha1.KafkaAPITLS{
				Enabled: true,
				IssuerRef: &cmmeta.ObjectReference{
					Name: "other",
					Kind: "ClusterIssuer",
				},
			}})

		err := newRp.ValidateUpdate(rpCluster)
		assert.Error(t, err)
	})

	// nolint:dupl // the tests are not duplicates
	t.Run("same issuer for two tls listeners is allowed", func(t *testing.T) {
		newRp := rpCluster.DeepCopy()
		newRp.Spec.Configuration.KafkaAPI[0].TLS = v1alpha1.KafkaAPITLS{
			Enabled: true,
			IssuerRef: &cmmeta.ObjectReference{
				Name: "issuer",
				Kind: "ClusterIssuer",
			},
		}
		newRp.Spec.Configuration.KafkaAPI = append(newRp.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{Port: 30001, External: v1alpha1.ExternalConnectivityConfig{Enabled: true, Subdomain: "redpanda.com"}, TLS: v1alpha1.KafkaAPITLS{
				Enabled: true,
				IssuerRef: &cmmeta.ObjectReference{
					Name: "issuer",
					Kind: "ClusterIssuer",
				},
			}})

		err := newRp.ValidateUpdate(rpCluster)
		assert.NoError(t, err)
	})

	// nolint:dupl // the tests are not duplicates
	t.Run("different nodeSecretRef for two tls listeners", func(t *testing.T) {
		newRp := rpCluster.DeepCopy()
		newRp.Spec.Configuration.KafkaAPI[0].TLS = v1alpha1.KafkaAPITLS{
			Enabled: true,
			NodeSecretRef: &corev1.ObjectReference{
				Name:      "node",
				Namespace: "default",
			},
		}
		newRp.Spec.Configuration.KafkaAPI = append(newRp.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{Port: 30001, External: v1alpha1.ExternalConnectivityConfig{Enabled: true, Subdomain: "redpanda.com"}, TLS: v1alpha1.KafkaAPITLS{
				Enabled: true,
				NodeSecretRef: &corev1.ObjectReference{
					Name:      "other-node",
					Namespace: "default",
				},
			}})

		err := newRp.ValidateUpdate(rpCluster)
		assert.Error(t, err)
	})

	// nolint:dupl // the tests are not duplicates
	t.Run("same nodesecretref for two tls listeners is allowed", func(t *testing.T) {
		newRp := rpCluster.DeepCopy()
		newRp.Spec.Configuration.KafkaAPI[0].TLS = v1alpha1.KafkaAPITLS{
			Enabled: true,
			NodeSecretRef: &corev1.ObjectReference{
				Name:      "node",
				Namespace: "default",
			},
		}
		newRp.Spec.Configuration.KafkaAPI = append(newRp.Spec.Configuration.KafkaAPI,
			v1alpha1.KafkaAPI{Port: 30001, External: v1alpha1.ExternalConnectivityConfig{Enabled: true, Subdomain: "redpanda.com"}, TLS: v1alpha1.KafkaAPITLS{
				Enabled: true,
				NodeSecretRef: &corev1.ObjectReference{
					Name:      "node",
					Namespace: "default",
				},
			}})

		err := newRp.ValidateUpdate(rpCluster)
		assert.NoError(t, err)
	})
}
