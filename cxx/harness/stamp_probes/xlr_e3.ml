module S : sig module R : sig type v end end = struct module rec R : sig type v
  = int end = struct type v = int end end
