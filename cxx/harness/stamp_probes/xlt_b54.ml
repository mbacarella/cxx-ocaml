module F (X : sig type t type u val v : t end) = struct module M = struct
  include X end end
module A = struct type t type u let v : t = assert false end
module N = F (A)
