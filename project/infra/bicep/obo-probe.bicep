targetScope = 'resourceGroup'

param location string
param environmentId string
param identityId string
param identityClientId string
param registryServer string
param image string
param apiClientId string
param deviceClientId string
param allowedUserOid string

resource probe 'Microsoft.App/jobs@2025-01-01' = {
  name: 'recorder-obo-probe'
  location: location
  tags: {
    application: 'embedded-recorder'
    purpose: 'manual-consumer-obo-proof'
  }
  identity: {
    type: 'UserAssigned'
    userAssignedIdentities: {
      '${identityId}': {}
    }
  }
  properties: {
    environmentId: environmentId
    workloadProfileName: 'Consumption'
    configuration: {
      triggerType: 'Manual'
      replicaTimeout: 900
      replicaRetryLimit: 0
      manualTriggerConfig: {
        parallelism: 1
        replicaCompletionCount: 1
      }
      registries: [
        {
          server: registryServer
          identity: identityId
        }
      ]
    }
    template: {
      containers: [
        {
          name: 'probe'
          image: image
          command: [
            'python'
            '-c'
          ]
          args: [
            loadTextContent('../scripts/probe_obo.py')
          ]
          env: [
            {
              name: 'AZURE_CLIENT_ID'
              value: identityClientId
            }
            {
              name: 'API_B_CLIENT_ID'
              value: apiClientId
            }
            {
              name: 'PUBLIC_CLIENT_A_ID'
              value: deviceClientId
            }
            {
              name: 'ALLOWED_USER_OID'
              value: allowedUserOid
            }
          ]
          resources: {
            cpu: json('0.25')
            memory: '0.5Gi'
          }
        }
      ]
    }
  }
}
output jobName string = probe.name
