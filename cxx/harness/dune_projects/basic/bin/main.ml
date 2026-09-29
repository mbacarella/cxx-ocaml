let () =
  let shapes = Mylib.Shape.[Circle 1.0; Rect (2., 3.)] in
  List.iter (fun s -> print_endline (Mylib.Shape.describe s)) shapes;
  Printf.printf "total %.2f\n" (Mylib.Util.sum (List.map Mylib.Shape.area shapes));
  Mylib.Util.M.iter (Printf.printf "%s=%d\n") (Mylib.Util.count ["a";"b";"a"])
