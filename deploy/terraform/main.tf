# One hardened EC2 instance for bitkv, plus an encrypted S3 bucket for its
# backups. Ansible (deploy/ansible) then configures the instance.

data "aws_vpc" "default" {
  default = true
}

data "aws_ami" "ubuntu" {
  most_recent = true
  owners      = ["099720109477"] # Canonical

  filter {
    name   = "name"
    values = ["ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*"]
  }
}

resource "aws_key_pair" "bitkv" {
  key_name_prefix = "bitkv-"
  public_key      = var.ssh_public_key
}

# ---- network: three ports, each open only to the network that needs it ----

resource "aws_security_group" "bitkv" {
  name_prefix = "bitkv-"
  description = "bitkv: SSH and metrics from admin, data port from clients"
  vpc_id      = data.aws_vpc.default.id

  lifecycle {
    create_before_destroy = true
  }
}

resource "aws_vpc_security_group_ingress_rule" "ssh" {
  security_group_id = aws_security_group.bitkv.id
  description       = "SSH from the admin IP only"
  cidr_ipv4         = var.admin_cidr
  ip_protocol       = "tcp"
  from_port         = 22
  to_port           = 22
}

resource "aws_vpc_security_group_ingress_rule" "data" {
  security_group_id = aws_security_group.bitkv.id
  description       = "bitkv data port from clients"
  cidr_ipv4         = var.client_cidr
  ip_protocol       = "tcp"
  from_port         = 6380
  to_port           = 6380
}

resource "aws_vpc_security_group_ingress_rule" "metrics" {
  security_group_id = aws_security_group.bitkv.id
  description       = "Prometheus metrics from the admin IP only"
  cidr_ipv4         = var.admin_cidr
  ip_protocol       = "tcp"
  from_port         = 9121
  to_port           = 9121
}

resource "aws_vpc_security_group_egress_rule" "all" {
  security_group_id = aws_security_group.bitkv.id
  description       = "Outbound for package installs and S3 backups"
  cidr_ipv4         = "0.0.0.0/0"
  ip_protocol       = "-1"
}

# ---- backups: private, encrypted, versioned --------------------------------

resource "aws_s3_bucket" "backups" {
  bucket_prefix = "bitkv-backups-"
}

resource "aws_s3_bucket_public_access_block" "backups" {
  bucket                  = aws_s3_bucket.backups.id
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

resource "aws_s3_bucket_server_side_encryption_configuration" "backups" {
  bucket = aws_s3_bucket.backups.id

  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

# Versioning means an overwritten or deleted backup can still be recovered.
resource "aws_s3_bucket_versioning" "backups" {
  bucket = aws_s3_bucket.backups.id

  versioning_configuration {
    status = "Enabled"
  }
}

resource "aws_s3_bucket_lifecycle_configuration" "backups" {
  bucket = aws_s3_bucket.backups.id

  rule {
    id     = "expire-old-versions"
    status = "Enabled"

    filter {}

    noncurrent_version_expiration {
      noncurrent_days = var.backup_retention_days
    }
  }
}

# ---- identity: least privilege ---------------------------------------------
# The instance may read and write objects under backups/ in ONE bucket, and
# register with SSM. Nothing else: no other buckets, no IAM, no EC2 API.

data "aws_iam_policy_document" "assume_ec2" {
  statement {
    actions = ["sts:AssumeRole"]

    principals {
      type        = "Service"
      identifiers = ["ec2.amazonaws.com"]
    }
  }
}

resource "aws_iam_role" "bitkv" {
  name_prefix        = "bitkv-"
  assume_role_policy = data.aws_iam_policy_document.assume_ec2.json
}

data "aws_iam_policy_document" "backups" {
  statement {
    sid       = "ReadWriteBackupObjects"
    actions   = ["s3:PutObject", "s3:GetObject"]
    resources = ["${aws_s3_bucket.backups.arn}/backups/*"]
  }

  statement {
    sid       = "ListOnlyTheBackupPrefix"
    actions   = ["s3:ListBucket"]
    resources = [aws_s3_bucket.backups.arn]

    condition {
      test     = "StringLike"
      variable = "s3:prefix"
      values   = ["backups/*"]
    }
  }
}

resource "aws_iam_role_policy" "backups" {
  name   = "bitkv-backups"
  role   = aws_iam_role.bitkv.id
  policy = data.aws_iam_policy_document.backups.json
}

# SSM Session Manager: shell access without opening port 22 at all. Once it
# works, the SSH ingress rule above can be deleted.
resource "aws_iam_role_policy_attachment" "ssm" {
  role       = aws_iam_role.bitkv.name
  policy_arn = "arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore"
}

resource "aws_iam_instance_profile" "bitkv" {
  name_prefix = "bitkv-"
  role        = aws_iam_role.bitkv.name
}

# ---- the instance ----------------------------------------------------------

resource "aws_instance" "bitkv" {
  ami                    = data.aws_ami.ubuntu.id
  instance_type          = var.instance_type
  key_name               = aws_key_pair.bitkv.key_name
  vpc_security_group_ids = [aws_security_group.bitkv.id]
  iam_instance_profile   = aws_iam_instance_profile.bitkv.name

  # IMDSv2 only. Session tokens defeat SSRF attacks that trick an app into
  # fetching the instance's IAM credentials from the metadata endpoint.
  metadata_options {
    http_endpoint               = "enabled"
    http_tokens                 = "required"
    http_put_response_hop_limit = 1
  }

  root_block_device {
    volume_type           = "gp3"
    volume_size           = 20
    encrypted             = true
    delete_on_termination = true
  }

  tags = {
    Name = "bitkv-1"
  }
}
