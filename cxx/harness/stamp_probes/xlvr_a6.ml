module M : sig type +'a t end = struct type 'a t = 'a list end
type 'a u = 'a M.t
