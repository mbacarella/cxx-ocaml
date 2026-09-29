module F (X : sig type t end) = struct type t = int let empty = 0 let card x = x
  end module I = struct type t = int end module C = F(I) let h = C.card C.empty
