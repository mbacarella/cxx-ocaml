module F (X : sig type t end) = struct module M = X end
module A = struct type t end
module N : sig module M : sig type t = A.t end end = F (A)
