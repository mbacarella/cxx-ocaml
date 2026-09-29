module type ORDERED = sig type t end module type HEAP = sig module Elem: ORDERED
  module Elem2: ORDERED end
