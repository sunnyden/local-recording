targetScope = 'resourceGroup'

param location string
@minLength(3)
@maxLength(18)
param namePrefix string
@allowed(['native', 'byom-azure-openai-realtime'])
param voiceProfile string = 'byom-azure-openai-realtime'
param modelName string = 'gpt-realtime-2'
param modelVersion string
@minValue(1)
param modelCapacity int = 1
@allowed(['GlobalStandard', 'DataZoneStandard', 'Standard'])
param modelSku string = 'GlobalStandard'

var suffix = uniqueString(subscription().id, resourceGroup().id, namePrefix)
var foundryName = '${namePrefix}-ai-${suffix}'
var tags = {
  application: 'embedded-recorder'
}

resource foundry 'Microsoft.CognitiveServices/accounts@2025-06-01' = {
  name: foundryName
  location: location
  kind: 'AIServices'
  tags: tags
  sku: {
    name: 'S0'
  }
  identity: {
    type: 'SystemAssigned'
  }
  properties: {
    customSubDomainName: foundryName
    allowProjectManagement: true
    disableLocalAuth: true
    publicNetworkAccess: 'Enabled'
  }
}

resource project 'Microsoft.CognitiveServices/accounts/projects@2025-06-01' = {
  parent: foundry
  name: 'recorder'
  location: location
  identity: {
    type: 'SystemAssigned'
  }
  properties: {
    displayName: 'Embedded recorder'
    description: 'Prototype Voice Live recorder project.'
  }
}

resource model 'Microsoft.CognitiveServices/accounts/deployments@2025-06-01' = if (voiceProfile != 'native') {
  parent: foundry
  name: modelName
  sku: {
    name: modelSku
    capacity: modelCapacity
  }
  properties: {
    model: {
      format: 'OpenAI'
      name: modelName
      version: modelVersion
    }
    versionUpgradeOption: 'NoAutoUpgrade'
  }
}

resource registry 'Microsoft.ContainerRegistry/registries@2023-07-01' = {
  name: '${replace(namePrefix, '-', '')}${suffix}'
  location: location
  tags: tags
  sku: {
    name: 'Basic'
  }
  properties: {
    adminUserEnabled: false
    publicNetworkAccess: 'Enabled'
  }
}

resource identity 'Microsoft.ManagedIdentity/userAssignedIdentities@2023-01-31' = {
  name: '${namePrefix}-proxy'
  location: location
  tags: tags
}

var roles = [
  'a97b65f3-24c7-4388-baec-2e87135dc908' // Cognitive Services User.
  '53ca6127-db72-4b80-b1b0-d745d6d5456d' // Foundry User.
]
resource foundryRoles 'Microsoft.Authorization/roleAssignments@2022-04-01' = [for role in roles: {
  name: guid(foundry.id, identity.id, role)
  scope: foundry
  properties: {
    roleDefinitionId: subscriptionResourceId('Microsoft.Authorization/roleDefinitions', role)
    principalId: identity.properties.principalId
    principalType: 'ServicePrincipal'
  }
}]

resource registryPull 'Microsoft.Authorization/roleAssignments@2022-04-01' = {
  name: guid(registry.id, identity.id, 'AcrPull')
  scope: registry
  properties: {
    roleDefinitionId: subscriptionResourceId('Microsoft.Authorization/roleDefinitions', '7f951dda-4ed3-4680-a7ca-43fe172d538d')
    principalId: identity.properties.principalId
    principalType: 'ServicePrincipal'
  }
}

resource environment 'Microsoft.App/managedEnvironments@2025-01-01' = {
  name: '${namePrefix}-environment'
  location: location
  tags: tags
  properties: {
    workloadProfiles: [
      {
        name: 'Consumption'
        workloadProfileType: 'Consumption'
      }
    ]
  }
}

output foundryResourceName string = foundry.name
output foundryResourceId string = foundry.id
output foundryProjectId string = project.id
output voiceLiveEndpoint string = 'https://${foundryName}.services.ai.azure.com'
output voiceLiveModel string = modelName
output voiceLiveProfile string = voiceProfile
output registryName string = registry.name
output registryServer string = registry.properties.loginServer
output proxyIdentityId string = identity.id
output proxyIdentityClientId string = identity.properties.clientId
output environmentId string = environment.id
