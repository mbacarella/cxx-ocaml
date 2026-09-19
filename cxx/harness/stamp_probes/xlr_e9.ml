module rec S : sig module R : sig type v end end = struct module rec R : sig
  type v = S.R.v end = struct type v = D end let z = 1 end
