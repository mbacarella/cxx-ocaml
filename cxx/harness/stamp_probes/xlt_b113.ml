module F (X : sig module N : sig type t type u end end) = struct module M = X
  end
module A = struct module N = struct type t type u end end
module R = F (A)
