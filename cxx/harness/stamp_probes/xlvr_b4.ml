module M = struct type ('a, 'b) t = 'a -> 'b end
type ('a, 'b) u = ('a, 'b) M.t
