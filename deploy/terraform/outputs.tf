output "public_ip" {
  description = "Public IP of the bitkv instance."
  value       = aws_instance.bitkv.public_ip
}

output "ssh_command" {
  description = "SSH into the instance."
  value       = "ssh ubuntu@${aws_instance.bitkv.public_ip}"
}

output "backup_bucket" {
  description = "S3 bucket for backups (objects under backups/)."
  value       = aws_s3_bucket.backups.bucket
}

output "ansible_inventory" {
  description = "Paste into deploy/ansible/inventory.ini."
  value       = <<-EOT
    [bitkv]
    bitkv-1 ansible_host=${aws_instance.bitkv.public_ip} ansible_user=ubuntu
  EOT
}
