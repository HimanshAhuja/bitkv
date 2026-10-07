variable "region" {
  description = "AWS region."
  type        = string
  default     = "ap-south-1" # Mumbai
}

variable "instance_type" {
  description = "EC2 instance type. t3.micro is free-tier eligible in most accounts."
  type        = string
  default     = "t3.micro"
}

variable "ssh_public_key" {
  description = "Contents of your SSH public key, e.g. file(\"~/.ssh/id_ed25519.pub\")."
  type        = string
}

variable "admin_cidr" {
  description = "Your IP as a /32. The only source allowed to SSH and scrape metrics."
  type        = string

  validation {
    condition     = can(cidrhost(var.admin_cidr, 0)) && var.admin_cidr != "0.0.0.0/0"
    error_message = "admin_cidr must be a valid CIDR and must not be 0.0.0.0/0."
  }
}

variable "client_cidr" {
  description = "Network allowed to reach the bitkv data port (6380)."
  type        = string
}

variable "backup_retention_days" {
  description = "Days to keep superseded backup versions."
  type        = number
  default     = 30
}
