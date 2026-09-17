module F (X : sig type t end) = struct module M = X end
module A = struct type t end
module P = struct module N = F (A) end
