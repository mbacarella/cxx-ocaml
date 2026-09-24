module M = struct type 'a t = { mutable x : 'a } end
type 'a u = U of 'a M.t
