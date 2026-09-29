module F (X : sig type t end) = struct module M = (X : sig type t = X.t end) end
module A = struct type t end
module N = F (A)
