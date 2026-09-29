let send_flush send ob oc t =
  send ob t;
  Buffer.output_buffer oc ob;
  Buffer.clear ob;
  flush oc
let send_string = send_flush (fun ob -> Printf.bprintf ob "%S\n");;
let receive_string ib = Scanf.bscanf ib "%S\n" (fun s -> s);;
