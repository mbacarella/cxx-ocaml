module F (X : sig module N : sig type t type u end end) = struct module M =
  struct include X end end
module A = struct module N = struct type t type u end end
module R = F (A)
