# Azam Car — AWS IoT + Lambda + Alexa setup script
# Run: .\setup-aws.ps1
# Pehle aws configure karo (Access Key + Secret Key + Region)

$ErrorActionPreference = "Stop"
$THING = "azam-car"
$REGION = "ap-south-1"
$ACCOUNT_ID = (aws sts get-caller-identity --query Account --output text 2>$null)
if (-not $ACCOUNT_ID) { Write-Host "ERROR: aws configure pehle karo (Access Key, Secret Key, Region)" -ForegroundColor Red; exit 1 }
$ARN_PREFIX = "arn:aws:iot:${REGION}:${ACCOUNT_ID}"

Write-Host "`n=== STEP 1: IoT Thing + Certs ===" -ForegroundColor Cyan

# Create thing
aws iot create-thing --thing-name $THING --region $REGION 2>$null
Write-Host "Thing '$THING' created/exists" -ForegroundColor Green

# Create certificate
$certOut = aws iot create-keys-and-certificate --set-as-active --region $REGION | ConvertFrom-Json
$certArn = $certOut.certificateArn
$certId = $certOut.certificateId
$certOut.certificatePem | Out-File -FilePath "device_cert.pem" -Encoding ascii
$certOut.privateKey | Out-File -FilePath "private_key.pem" -Encoding ascii
Write-Host "Certs saved: device_cert.pem, private_key.pem" -ForegroundColor Green

# Download Amazon Root CA
Invoke-WebRequest -Uri "https://www.amazontrust.com/repository/AmazonRootCA1.pem" -OutFile "AmazonRootCA1.pem" -UseBasicParsing
Write-Host "Amazon Root CA downloaded" -ForegroundColor Green

# Create policy
$policyDoc = @"
{
  "Version": "2012-10-17",
  "Statement": [
    {"Effect":"Allow","Action":"iot:Connect","Resource":"${ARN_PREFIX}:client/${THING}"},
    {"Effect":"Allow","Action":"iot:Publish","Resource":["${ARN_PREFIX}:topic/azamcar/resp/*","${ARN_PREFIX}:topic/azamcar/event/*","${ARN_PREFIX}:topic/azamcar/state/*","${ARN_PREFIX}:topic/`$aws/things/${THING}/shadow/update"]},
    {"Effect":"Allow","Action":["iot:Subscribe","iot:Receive"],"Resource":"${ARN_PREFIX}:topicfilter/azamcar/cmd/*"}
  ]
}
"@
$policyDoc | Out-File -FilePath "policy.json" -Encoding ascii
aws iot create-policy --policy-name "${THING}-policy" --policy-document file://policy.json --region $REGION 2>$null
Write-Host "Policy '${THING}-policy' created" -ForegroundColor Green

# Attach policy to cert
aws iot attach-policy --policy-name "${THING}-policy" --target $certArn --region $REGION 2>$null
Write-Host "Policy attached to certificate" -ForegroundColor Green

# Attach cert to thing
aws iot attach-thing-principal --thing-name $THING --principal $certArn --region $REGION 2>$null
Write-Host "Certificate attached to thing" -ForegroundColor Green

# Get IoT endpoint
$endpoint = (aws iot describe-endpoint --endpoint-type "iot:Data-ATS" --region $REGION | ConvertFrom-Json).endpointAddress
Write-Host "`nIoT Endpoint: $endpoint" -ForegroundColor Yellow

Write-Host "`n=== STEP 2: Lambda Function ===" -ForegroundColor Cyan

# Create IAM role for Lambda
$trustPolicy = '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"Service":"lambda.amazonaws.com"},"Action":"sts:AssumeRole"}]}'
$trustPolicy | Out-File -FilePath "trust.json" -Encoding ascii
aws iam create-role --role-name azam-car-lambda-role --assume-role-policy-document file://trust.json 2>$null | Out-Null
Write-Host "IAM role created" -ForegroundColor Green

# Attach basic execution policy
aws iam attach-role-policy --role-name azam-car-lambda-role --policy-arn "arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole" 2>$null

# Create inline IoT policy for Lambda
$lambdaPolicy = @"
{
  "Version":"2012-10-17",
  "Statement":[
    {"Effect":"Allow","Action":["iot:Publish","iot:Subscribe"],"Resource":"${ARN_PREFIX}:topic/azamcar/*"},
    {"Effect":"Allow","Action":["iot:GetThingShadow"],"Resource":"${ARN_PREFIX}:thing/${THING}"}
  ]
}
"@
$lambdaPolicy | Out-File -FilePath "lambda_policy.json" -Encoding ascii
aws iam put-role-policy --role-name azam-car-lambda-role --policy-name iot-access --policy-document file://lambda_policy.json 2>$null
Write-Host "Lambda IoT policy attached" -ForegroundColor Green

Start-Sleep -Seconds 10

# Create Lambda function
$roleArn = "arn:aws:iam::${ACCOUNT_ID}:role/azam-car-lambda-role"
$zipPath = Join-Path $PSScriptRoot "function.zip"
aws lambda create-function --function-name azam-car-alexa --runtime nodejs20.x --role $roleArn --handler lambda_index.handler --zip-file "fileb://$zipPath" --timeout 5 --region $REGION --environment "Variables={IOT_ENDPOINT=$endpoint,USER_ID=default,THING_NAME=$THING,WAIT_MS=1800}" 2>$null | Out-Null
Write-Host "Lambda function created" -ForegroundColor Green

$lambdaArn = "arn:aws:lambda:${REGION}:${ACCOUNT_ID}:function:azam-car-alexa"
Write-Host "`nLambda ARN: $lambdaArn" -ForegroundColor Yellow

Write-Host "`n=== STEP 3: Alexa Skill Instructions ===" -ForegroundColor Cyan
Write-Host @"

Ab Alexa Developer Console pe manually karo:
1. https://developer.amazon.com/alexa/console/ask
2. Create Skill -> Name: Azam Car, Locale: English (IN)
3. Type: Custom, Hosting: Provision your own, Template: Start from scratch
4. Left sidebar -> Interaction Model -> JSON Editor
   Paste content from: cloud/alexa/interactionModel_en-IN.json
5. Save Model -> Build Model
6. Left sidebar -> Endpoint -> AWS Lambda ARN
   Paste: $lambdaArn
7. Save Endpoints
8. Account Linking -> Do not require account linking -> Save

Test: "Alexa, ask Azam Car for status"
"@ -ForegroundColor White

Write-Host "`n=== DONE ===" -ForegroundColor Green
Write-Host "Next: flash car with MQTT broker set to: mqtts://${endpoint}:8883" -ForegroundColor Yellow
