module F (X : sig type t end) = struct module M = struct module L = struct
  module K = X end end end
