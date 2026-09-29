module M = struct type 'a t = 'a -> unit end
module M2 = struct type 'a t = 'a list end
type 'a u = 'a M.t * 'a M2.t
