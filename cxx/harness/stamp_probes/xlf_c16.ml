module type S = sig module Q : sig type t end val v : Q.t end
module Register (D : S) = struct let x = D.v end
module N = struct module Q = struct type t = int end let v = 1 end
module X = Register (N)
