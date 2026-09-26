module F (X: sig type t end) : sig module N : sig type w val z : w end
  end = struct module N = struct type w = int let z = 0 end end module A =
  struct type t = bool end module C = F(A) let z = C.N.z
