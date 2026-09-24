module M = struct type 'a t = A of 'a end
module N = M
type 'a u = 'a N.t
