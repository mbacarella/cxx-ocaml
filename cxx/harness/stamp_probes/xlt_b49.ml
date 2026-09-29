module F (X : sig type t end) = struct module M = X end
module A = struct type t = int end
module N = F (A)
