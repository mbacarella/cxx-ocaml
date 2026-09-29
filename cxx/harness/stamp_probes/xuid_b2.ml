type _ t =
 | Float : float t
 | String : string t
let f : type a . a t -> a -> unit = fun t a -> ()
let g1 = 0
let f2 : type a . a t -> a -> unit = fun t a ->
 match t with
 | Float -> ()
 | String -> ()
let g2 = 0
let f3 : type a . a t -> a -> unit = fun t a ->
 match t with
 | Float -> ()
 | String -> ignore (String.length a : int)
let g3 = 0
let f4 (type a) (t : a t) (a : a) : unit =
 match t with
 | Float -> ()
 | String -> ()
let g4 = 0
let f5 (type a) (t : a t) (a : a) : unit = ()
let g5 = 0
