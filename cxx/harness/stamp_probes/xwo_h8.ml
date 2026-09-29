module type ORDERED = sig type t end module type ZZ = sig module Z : ORDERED end
  module type HEAP = sig module Elem: ZZ end
