targetScope = 'resourceGroup'

param location string
param namePrefix string
param environmentId string
param identityId string
param identityClientId string
param registryServer string
param image string
param voiceLiveEndpoint string
param voiceLiveModel string
param voiceLiveProfile string

var probe = '''
import asyncio
import os
from types import SimpleNamespace
from recorder_proxy.providers import VoiceLive

async def main():
    settings = SimpleNamespace(
        managed_identity_client_id=os.environ["AZURE_CLIENT_ID"],
        endpoint=os.environ["VOICELIVE_ENDPOINT"],
        model=os.environ["VOICELIVE_MODEL"],
        profile=os.environ["VOICELIVE_PROFILE"],
        handshake_seconds=30,
    )
    provider = VoiceLive(settings)
    try:
        await provider.open()
        print("MANAGED_IDENTITY_VOICELIVE_CONFIGURATION_ACCEPTED", flush=True)
    finally:
        await provider.close()

asyncio.run(main())
'''

resource job 'Microsoft.App/jobs@2025-01-01' = {
  name: '${namePrefix}-identity-probe'
  location: location
  tags: {
    application: 'embedded-recorder'
    purpose: 'manual-keyless-validation'
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
      replicaTimeout: 90
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
            probe
          ]
          env: [
            {
              name: 'AZURE_CLIENT_ID'
              value: identityClientId
            }
            {
              name: 'VOICELIVE_ENDPOINT'
              value: voiceLiveEndpoint
            }
            {
              name: 'VOICELIVE_MODEL'
              value: voiceLiveModel
            }
            {
              name: 'VOICELIVE_PROFILE'
              value: voiceLiveProfile
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

output jobName string = job.name
