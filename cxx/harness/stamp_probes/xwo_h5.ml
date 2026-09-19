module type ORDERED = sig type t end module type HEAP = sig module Elem: sig
  type t end end
