type _ t = Float : float t | String : string t | Int : int t
let h1 (type a) (t : a t) =
  (match t with Float -> () | _ -> ()); (match t with Float -> () | _ -> ())
let m1 = 0
let h2 (type a) (t : a t) =
  (match t with Float -> () | _ -> ()); (match t with String -> () | _ -> ())
let m2 = 0
let h3 (type a) (type b) (t : a t) (u : b t) =
  (match t with Float -> () | _ -> ());
  (match u with String -> () | _ -> ())
let m3 = 0
let h4 (type a) (t : a t) = match t with Float -> ()
let m4 = 0
let h5 (type a) (t : a t) =
  match t with Float -> () | String -> () | Int -> () | Float -> ()
let m5 = 0
let h6 (type a) (t : a t) = match t with Float -> 1 | _ -> 2
let m6 = 0
let h7 (type a) (t : a t) =
  match t with
  | Float -> (fun (x : a) -> x +. 1.)
  | _ -> (fun x -> ignore x; 1.)
let m7 = 0
let h8 (type a) (t : a t) = match t with (Float | Int) -> () | _ -> ()
let m8 = 0
let h9 (type a) (t : a t list) = match t with [Float] -> () | _ -> ()
let m9 = 0
let h10 (type a) (t : a t) = match t with Float when true -> () | _ -> ()
let m10 = 0
let h11 (type a) (t : a t) = match t with Float | _ -> ()
let m11 = 0
let h12 (type a) (t : a t) = match t with Float -> () | String -> () | _ -> ()
let m12 = 0
