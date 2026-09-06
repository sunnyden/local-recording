targetScope = 'resourceGroup'

param location string
param namePrefix string
param environmentId string
param identityId string
param registryServer string
@description('Immutable private image reference, preferably including a digest.')
param image string
@description('Nonsecret application configuration only. Never supply access tokens or API keys.')
param environmentVariables object
param healthPath string = '/healthz'
param readinessPath string = '/readyz'

resource proxy 'Microsoft.App/containerApps@2025-01-01' = {
  name: '${namePrefix}-proxy'
  location: location
  tags: {
    application: 'embedded-recorder'
  }
  identity: {
    type: 'UserAssigned'
    userAssignedIdentities: {
      '${identityId}': {}
    }
  }
  properties: {
    managedEnvironmentId: environmentId
    workloadProfileName: 'Consumption'
    configuration: {
      activeRevisionsMode: 'Single'
      ingress: {
        external: true
        allowInsecure: false
        targetPort: 8000
        transport: 'http'
      }
      registries: [
        {
          server: registryServer
          identity: identityId
        }
      ]
    }
    template: {
      terminationGracePeriodSeconds: 30
      containers: [
        {
          name: 'proxy'
          image: image
          resources: {
            cpu: json('0.25')
            memory: '0.5Gi'
          }
          env: [for entry in items(environmentVariables): {
            name: entry.key
            value: string(entry.value)
          }]
          probes: [
            {
              type: 'Startup'
              httpGet: {
                path: readinessPath
                port: 8000
              }
              periodSeconds: 5
              failureThreshold: 30
            }
            {
              type: 'Readiness'
              httpGet: {
                path: readinessPath
                port: 8000
              }
              periodSeconds: 10
              failureThreshold: 3
            }
            {
              type: 'Liveness'
              httpGet: {
                path: healthPath
                port: 8000
              }
              periodSeconds: 30
              failureThreshold: 3
            }
          ]
        }
      ]
      scale: {
        minReplicas: 0
        maxReplicas: 1
        rules: [
          {
            name: 'http'
            http: {
              metadata: {
                concurrentRequests: '1'
              }
            }
          }
        ]
      }
    }
  }
}

output proxyUrl string = 'https://${proxy.properties.configuration.ingress.fqdn}'
output voiceWebSocketUrl string = 'wss://${proxy.properties.configuration.ingress.fqdn}/v1/voice'
output proxyResourceId string = proxy.id
