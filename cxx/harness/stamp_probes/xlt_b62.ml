module F (X : sig type t end) = struct module M = struct module L = struct
  include X end end end
