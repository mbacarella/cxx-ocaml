module F (X : sig type t end) = struct module M = struct module L = struct
  include X end end end
module A = struct type t end
module N = F (A)
