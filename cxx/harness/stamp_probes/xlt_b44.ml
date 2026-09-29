module F (X : sig type t end) = struct module M : sig type t = X.t end = X end
module A = struct type t end
module N = F (A)
