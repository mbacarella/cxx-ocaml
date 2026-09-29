module List = struct type 'a t = A of 'a ref end
type 'a u = 'a List.t
