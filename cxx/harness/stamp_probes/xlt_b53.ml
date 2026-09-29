module F (X : sig type t type u val v : t end) = struct module M = struct
  include X end end
module N = F (struct type t type u let v : t = assert false end)
