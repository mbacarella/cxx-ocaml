module rec S : sig module R : sig type v end end = struct module rec R : sig
  type v = S.R.v type w end = struct type v = D type w end end
