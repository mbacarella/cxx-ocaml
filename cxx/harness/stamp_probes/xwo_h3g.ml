module type ORDERED = sig type t end module type HEAP = sig module Elem: ORDERED
  end module M = struct type t = int end module type MK = HEAP with module Elem
  = M
