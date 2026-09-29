module P = struct module F (X : sig type t end) = struct module M = struct
  module L = X end end end
