targetScope = 'resourceGroup'

param location string
param environmentId string
param identityId string
param identityClientId string
param registryServer string
param image string
param speechEndpoint string

resource probe 'Microsoft.App/jobs@2025-01-01' = {
  name: 'recorder-speech-probe'
  location: location
  tags: {
    application: 'embedded-recorder'
    purpose: 'manual-fast-transcription-proof'
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
      replicaTimeout: 180
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
            loadTextContent('../scripts/probe_speech.py')
          ]
          env: [
            {
              name: 'AZURE_CLIENT_ID'
              value: identityClientId
            }
            {
              name: 'SPEECH_ENDPOINT'
              value: speechEndpoint
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
