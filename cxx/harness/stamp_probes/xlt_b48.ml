module F (X : sig type t type u end) = struct module M = X end
module A = struct type t type u end
module N = F (A)
