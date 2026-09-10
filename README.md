# Flying Pet

Mod nativo C++20 para Minecraft Bedrock no Android via LeviLauncher.

O mod desenha um pequeno pet 3D cliente-side que voa perto do jogador, balança verticalmente, bate as asas e acompanha o jogador com movimento suavizado. Se o jogador se afastar muito, o pet acelera para alcançar.

## Requisitos

- Android arm64-v8a
- LeviLauncher
- Minecraft Bedrock compatível com a versão do BedrockTools instalada
- BedrockTools instalado e habilitado

O Flying Pet usa a API runtime do `libBedrockTools.so` para resolver as funções internas do Minecraft. Isso evita colocar signatures fixas do `libminecraftpe.so` diretamente neste protótipo.

## Instalação

1. Instale e habilite o BedrockTools no LeviLauncher.
2. Abra a aba **Actions** deste repositório.
3. Entre no workflow **Build Flying Pet**.
4. Baixe o artifact `FlyingPet-arm64-v8a` da execução concluída.
5. Extraia `FlyingPet.levipack` do artifact.
6. Importe o `.levipack` pelo gerenciador de mods do LeviLauncher.
7. Deixe BedrockTools e Flying Pet habilitados e abra o Minecraft pelo LeviLauncher.

## Como o pet funciona

- O jogador local é obtido por `ClientInstanceGetLocalPlayer`.
- A posição é lida do `StateVectorComponent` do Actor.
- O alvo do pet orbita aproximadamente 1,55 bloco ao redor do jogador.
- O pet fica cerca de 1,75 bloco acima da posição do jogador.
- Um `lerp` suaviza o movimento.
- O pet é renderizado no `RenderLevel` usando o Tessellator do Minecraft.
- As asas usam uma animação senoidal simples.

## Limitações da v0.1.0

Este primeiro protótipo é **visual e cliente-side**. Ele não é uma entidade real do mundo: outros jogadores não veem o pet, ele não tem colisão, vida, inventário nem IA do servidor.

Também depende dos offsets/signatures que a versão instalada do BedrockTools suporta. Se o Minecraft atualizar e o BedrockTools ainda não suportar a nova versão, o pet pode não iniciar.

## Próximos passos possíveis

- Menu no LeviLauncher para ativar/desativar o pet
- Escolher distância, altura e velocidade
- Pet ficar atrás ou ao lado do jogador em vez de orbitar
- Textura/modelo próprio
- Mais tipos de pet
- Nome acima do pet
- Animações extras
- Versão independente do BedrockTools

## Créditos técnicos

O projeto usa a API pública/runtime do BedrockTools como referência para ClientInstance, signatures, offsets e renderização no Minecraft Bedrock Android.
