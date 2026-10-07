terraform {
  required_version = ">= 1.6"

  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.60"
    }
  }

  # State is local by default. For a team, store it remotely with locking:
  # backend "s3" {
  #   bucket       = "my-terraform-state"
  #   key          = "bitkv/terraform.tfstate"
  #   region       = "ap-south-1"
  #   use_lockfile = true
  # }
}

provider "aws" {
  region = var.region

  default_tags {
    tags = {
      Project   = "bitkv"
      ManagedBy = "terraform"
    }
  }
}
